#include "session_log.h"

#include <SDL2/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string_view>
#include <thread>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

#ifndef MM_GIT_COMMIT
#define MM_GIT_COMMIT "unknown"
#endif
#ifndef MM_BUILD_TYPE
#define MM_BUILD_TYPE "unknown"
#endif

namespace mm::session_log {
namespace {

constexpr size_t kMaximumLogBytes = 512 * 1024;
constexpr size_t kCompactionSlack = 64 * 1024;
constexpr size_t kRetainedPrefixBytes = 32 * 1024;
constexpr std::string_view kCompactionMarker =
    "\n[log] ... older session output was discarded ...\n";

std::mutex g_state_mutex;
std::mutex g_file_mutex;
std::ofstream g_log_file;
std::thread g_reader_thread;
std::filesystem::path g_log_dir;
std::filesystem::path g_current_path;
std::filesystem::path g_previous_path;
std::string g_header;
size_t g_file_size = 0;
std::chrono::steady_clock::time_point g_log_started_at;
bool g_at_line_start = true;
bool g_started = false;
bool g_redirected = false;
bool g_atexit_registered = false;
bool g_streams_unbuffered = false;
int g_pipe_read = -1;
int g_saved_stdout = -1;
int g_saved_stderr = -1;
int g_stdout_fd = -1;
int g_stderr_fd = -1;

#if defined(_WIN32)
int close_fd(int fd) { return _close(fd); }
int read_fd(int fd, void* buffer, unsigned int size) {
    return _read(fd, buffer, size);
}
int write_fd(int fd, const void* buffer, unsigned int size) {
    return _write(fd, buffer, size);
}
int duplicate_fd(int fd) { return _dup(fd); }
int replace_fd(int source, int destination) {
    return _dup2(source, destination);
}
#else
int close_fd(int fd) { return close(fd); }
ssize_t read_fd(int fd, void* buffer, size_t size) {
    return read(fd, buffer, size);
}
ssize_t write_fd(int fd, const void* buffer, size_t size) {
    return write(fd, buffer, size);
}
int duplicate_fd(int fd) { return dup(fd); }
int replace_fd(int source, int destination) {
    return dup2(source, destination) < 0 ? -1 : 0;
}
#endif

const char* platform_name() {
#if defined(_WIN32)
    return "Windows";
#elif defined(__APPLE__)
    return "macOS";
#elif defined(__linux__)
    return "Linux";
#else
    return "Unknown";
#endif
}

const char* architecture_name() {
#if defined(_M_X64) || defined(__x86_64__)
    return "x86_64";
#elif defined(_M_ARM64) || defined(__aarch64__)
    return "arm64";
#elif defined(_M_IX86) || defined(__i386__)
    return "x86";
#else
    return "unknown";
#endif
}

std::string timestamp_now() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    std::ostringstream stream;
    stream << std::put_time(&local, "%Y-%m-%d %H:%M:%S %z");
    return stream.str();
}

std::string make_header(const std::string& version) {
    SDL_version compiled{};
    SDL_version runtime{};
    SDL_VERSION(&compiled);
    SDL_GetVersion(&runtime);

    std::error_code ec;
    const bool portable = std::filesystem::exists("portable.txt", ec) ||
                          std::getenv("APP_FOLDER_PATH") != nullptr;

    std::ostringstream stream;
    stream << "Trouble Makers recompilation diagnostic log\n"
           << "Version: " << version << '\n'
           << "Commit: " << MM_GIT_COMMIT << '\n'
           << "Build: " << MM_BUILD_TYPE << '\n'
           << "Started: " << timestamp_now() << '\n'
           << "Platform: " << platform_name() << " (" << architecture_name() << ")\n"
           << "SDL: compiled " << static_cast<int>(compiled.major) << '.'
           << static_cast<int>(compiled.minor) << '.' << static_cast<int>(compiled.patch)
           << ", runtime " << static_cast<int>(runtime.major) << '.'
           << static_cast<int>(runtime.minor) << '.' << static_cast<int>(runtime.patch) << '\n'
           << "Portable mode: " << (portable ? "yes" : "no") << '\n'
           << "============================================================\n";
    return stream.str();
}

void write_all_to_console(const char* data, size_t size) {
    const int fd = g_saved_stderr >= 0 ? g_saved_stderr : g_saved_stdout;
    if (fd < 0) return;
    size_t offset = 0;
    while (offset < size) {
#if defined(_WIN32)
        const unsigned int chunk = static_cast<unsigned int>(
            std::min<size_t>(size - offset, static_cast<size_t>(INT_MAX)));
        const int written = write_fd(fd, data + offset, chunk);
#else
        const ssize_t written = write_fd(fd, data + offset, size - offset);
#endif
        if (written <= 0) break;
        offset += static_cast<size_t>(written);
    }
}

void write_timestamped_locked(const char* data, size_t size) {
    size_t offset = 0;
    while (offset < size) {
        if (g_at_line_start) {
            const double elapsed = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - g_log_started_at).count();
            std::array<char, 32> prefix{};
            const int count = std::snprintf(
                prefix.data(), prefix.size(), "[+%09.3fs] ", elapsed);
            const size_t prefix_size = count > 0
                ? std::min<size_t>(static_cast<size_t>(count), prefix.size() - 1)
                : 0;
            g_log_file.write(prefix.data(),
                static_cast<std::streamsize>(prefix_size));
            g_file_size += prefix_size;
            g_at_line_start = false;
        }

        const void* newline_ptr = std::memchr(data + offset, '\n', size - offset);
        const size_t segment_size = newline_ptr != nullptr
            ? static_cast<const char*>(newline_ptr) - (data + offset) + 1
            : size - offset;
        g_log_file.write(data + offset,
            static_cast<std::streamsize>(segment_size));
        g_file_size += segment_size;
        offset += segment_size;
        if (newline_ptr != nullptr) g_at_line_start = true;
    }
}

void compact_locked() {
    if (g_file_size <= kMaximumLogBytes || g_current_path.empty()) return;

    g_log_file.flush();
    g_log_file.close();

    std::string prefix;
    std::string tail;
    std::ifstream source(g_current_path, std::ios::binary | std::ios::ate);
    if (source.is_open()) {
        const std::streamoff end = source.tellg();
        const size_t total = end > 0 ? static_cast<size_t>(end) : 0;
        const size_t prefix_size = std::min(total, kRetainedPrefixBytes);
        prefix.resize(prefix_size);
        source.seekg(0);
        source.read(prefix.data(), static_cast<std::streamsize>(prefix.size()));
        prefix.resize(static_cast<size_t>(source.gcount()));

        const size_t fixed_size = prefix.size() + kCompactionMarker.size();
        const size_t tail_capacity = kMaximumLogBytes > fixed_size
            ? kMaximumLogBytes - fixed_size : 0;
        const size_t tail_start = total > tail_capacity
            ? std::max(prefix.size(), total - tail_capacity)
            : prefix.size();
        if (tail_start < total) {
            source.clear();
            source.seekg(static_cast<std::streamoff>(tail_start));
            tail.assign(std::istreambuf_iterator<char>(source),
                        std::istreambuf_iterator<char>());
        }
    }

    g_log_file.open(g_current_path, std::ios::binary | std::ios::trunc);
    if (!g_log_file.is_open()) {
        g_file_size = 0;
        return;
    }
    if (prefix.empty()) prefix = g_header;
    g_log_file.write(prefix.data(), static_cast<std::streamsize>(prefix.size()));
    g_log_file.write(kCompactionMarker.data(),
                     static_cast<std::streamsize>(kCompactionMarker.size()));
    g_log_file.write(tail.data(), static_cast<std::streamsize>(tail.size()));
    g_log_file.flush();
    g_file_size = prefix.size() + kCompactionMarker.size() + tail.size();
}

void reader_main() {
    std::array<char, 16 * 1024> buffer{};
    for (;;) {
#if defined(_WIN32)
        const int count = read_fd(g_pipe_read, buffer.data(),
                                  static_cast<unsigned int>(buffer.size()));
#else
        const ssize_t count = read_fd(g_pipe_read, buffer.data(), buffer.size());
#endif
        if (count <= 0) break;

        const size_t size = static_cast<size_t>(count);
        write_all_to_console(buffer.data(), size);
        std::lock_guard lock(g_file_mutex);
        if (g_log_file.is_open()) {
            write_timestamped_locked(buffer.data(), size);
            g_log_file.flush();
            if (g_file_size > kMaximumLogBytes + kCompactionSlack) {
                compact_locked();
            }
        }
    }
}

bool start_redirect() {
    std::fflush(stdout);
    std::fflush(stderr);

    int pipe_fds[2] = {-1, -1};
#if defined(_WIN32)
    int stdout_fd = _fileno(stdout);
    int stderr_fd = _fileno(stderr);
    // GUI-subsystem builds start with detached CRT streams (_fileno returns a
    // negative sentinel). Reopen those streams first so fprintf/printf have a
    // real descriptor that can be redirected into the pipe.
    FILE* reopened = nullptr;
    if (stdout_fd < 0 && freopen_s(&reopened, "NUL", "w", stdout) == 0) {
        stdout_fd = _fileno(stdout);
    }
    reopened = nullptr;
    if (stderr_fd < 0 && freopen_s(&reopened, "NUL", "w", stderr) == 0) {
        stderr_fd = _fileno(stderr);
    }
    if (stdout_fd < 0 || stderr_fd < 0) return false;
    if (_pipe(pipe_fds, 64 * 1024, _O_BINARY) != 0) return false;
#else
    if (pipe(pipe_fds) != 0) return false;
    const int stdout_fd = fileno(stdout);
    const int stderr_fd = fileno(stderr);
#endif

    if (stdout_fd < 0 || stderr_fd < 0) {
        close_fd(pipe_fds[0]);
        close_fd(pipe_fds[1]);
        return false;
    }
    g_stdout_fd = stdout_fd;
    g_stderr_fd = stderr_fd;

    g_saved_stdout = duplicate_fd(stdout_fd);
    g_saved_stderr = duplicate_fd(stderr_fd);
    if (g_saved_stdout < 0 || g_saved_stderr < 0) {
        if (g_saved_stdout >= 0) close_fd(g_saved_stdout);
        if (g_saved_stderr >= 0) close_fd(g_saved_stderr);
        g_saved_stdout = g_saved_stderr = -1;
        close_fd(pipe_fds[0]);
        close_fd(pipe_fds[1]);
        return false;
    }
    if (replace_fd(pipe_fds[1], stdout_fd) != 0 ||
        replace_fd(pipe_fds[1], stderr_fd) != 0) {
        if (g_saved_stdout >= 0) replace_fd(g_saved_stdout, stdout_fd);
        if (g_saved_stderr >= 0) replace_fd(g_saved_stderr, stderr_fd);
        close_fd(pipe_fds[0]);
        close_fd(pipe_fds[1]);
        return false;
    }

    close_fd(pipe_fds[1]);
    g_pipe_read = pipe_fds[0];
    if (!g_streams_unbuffered) {
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        std::setvbuf(stderr, nullptr, _IONBF, 0);
        g_streams_unbuffered = true;
    }
    clearerr(stdout);
    clearerr(stderr);
    g_reader_thread = std::thread(reader_main);
    return true;
}

std::string read_bounded(const std::filesystem::path& path, size_t max_bytes) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return {};
    const std::streamoff end = file.tellg();
    if (end <= 0) return {};

    const size_t total = static_cast<size_t>(end);
    if (total <= max_bytes) {
        file.seekg(0);
        return std::string(std::istreambuf_iterator<char>(file),
                           std::istreambuf_iterator<char>());
    }

    constexpr std::string_view marker =
        "\n[report] ... middle of log omitted for GitHub paste ...\n";
    const size_t prefix_size = std::min<size_t>(8 * 1024, max_bytes / 3);
    const size_t tail_size = max_bytes > prefix_size + marker.size()
        ? max_bytes - prefix_size - marker.size() : 0;
    std::string report(prefix_size, '\0');
    file.seekg(0);
    file.read(report.data(), static_cast<std::streamsize>(prefix_size));
    report.resize(static_cast<size_t>(file.gcount()));
    report.append(marker);
    if (tail_size > 0) {
        file.clear();
        file.seekg(end - static_cast<std::streamoff>(tail_size));
        report.append(std::istreambuf_iterator<char>(file),
                      std::istreambuf_iterator<char>());
    }
    return report;
}

} // namespace

bool start(const std::filesystem::path& app_folder, const std::string& version) {
    std::lock_guard state_lock(g_state_mutex);
    if (g_started) return true;

    g_log_dir = app_folder / "logs";
    g_current_path = g_log_dir / "latest.log";
    g_previous_path = g_log_dir / "previous.log";
    std::error_code ec;
    std::filesystem::create_directories(g_log_dir, ec);
    if (ec) return false;

    std::filesystem::remove(g_previous_path, ec);
    ec.clear();
    if (std::filesystem::exists(g_current_path, ec)) {
        ec.clear();
        std::filesystem::rename(g_current_path, g_previous_path, ec);
        if (ec) {
            ec.clear();
            std::filesystem::copy_file(g_current_path, g_previous_path,
                std::filesystem::copy_options::overwrite_existing, ec);
            if (!ec) std::filesystem::remove(g_current_path, ec);
        }
    }

    g_header = make_header(version);
    g_log_started_at = std::chrono::steady_clock::now();
    g_at_line_start = true;
    g_log_file.open(g_current_path, std::ios::binary | std::ios::trunc);
    if (!g_log_file.is_open()) return false;
    g_log_file.write(g_header.data(), static_cast<std::streamsize>(g_header.size()));
    g_log_file.flush();
    g_file_size = g_header.size();

    g_redirected = start_redirect();
    g_started = true;
    if (!g_atexit_registered) {
        std::atexit(stop);
        g_atexit_registered = true;
    }

    if (g_redirected) {
        std::fprintf(stderr, "[log] Capturing this session in logs/latest.log.\n");
    } else {
        std::lock_guard file_lock(g_file_mutex);
        g_log_file << "[log] stdout/stderr capture could not be initialized.\n";
        g_log_file.flush();
        g_file_size = static_cast<size_t>(g_log_file.tellp());
    }
    return g_redirected;
}

void flush() {
    std::fflush(stdout);
    std::fflush(stderr);
    std::lock_guard lock(g_file_mutex);
    if (g_log_file.is_open()) g_log_file.flush();
}

void stop() {
    std::unique_lock state_lock(g_state_mutex);
    if (!g_started) return;
    g_started = false;

    std::fflush(stdout);
    std::fflush(stderr);
    if (g_redirected) {
        if (g_saved_stdout >= 0) replace_fd(g_saved_stdout, g_stdout_fd);
        if (g_saved_stderr >= 0) replace_fd(g_saved_stderr, g_stderr_fd);
    }
    state_lock.unlock();

    if (g_reader_thread.joinable()) g_reader_thread.join();
    if (g_pipe_read >= 0) close_fd(g_pipe_read);
    if (g_saved_stdout >= 0) close_fd(g_saved_stdout);
    if (g_saved_stderr >= 0) close_fd(g_saved_stderr);
    g_pipe_read = g_saved_stdout = g_saved_stderr = -1;
    g_stdout_fd = g_stderr_fd = -1;

    std::lock_guard file_lock(g_file_mutex);
    compact_locked();
    if (g_log_file.is_open()) g_log_file.close();
    g_redirected = false;
}

std::filesystem::path log_directory() {
    return g_log_dir;
}

std::filesystem::path log_path(Session session) {
    return session == Session::Current ? g_current_path : g_previous_path;
}

bool has_report(Session session) {
    std::error_code ec;
    const auto path = log_path(session);
    if (path.empty() || !std::filesystem::exists(path, ec) || ec) return false;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    return !ec && size > 0;
}

std::string read_report(Session session, size_t max_bytes) {
    if (max_bytes < 1024) max_bytes = 1024;
    if (session == Session::Current) {
        flush();
        std::lock_guard lock(g_file_mutex);
        compact_locked();
        return read_bounded(g_current_path, max_bytes);
    }
    return read_bounded(g_previous_path, max_bytes);
}

} // namespace mm::session_log
