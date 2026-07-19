#include "session_log.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

std::string read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file),
                       std::istreambuf_iterator<char>());
}

int check(bool condition, const char* message) {
    if (condition) return 0;
    std::fprintf(stderr, "session log test failed: %s\n", message);
    return 1;
}

} // namespace

int main() {
    using namespace mm::session_log;
    namespace fs = std::filesystem;

    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() /
        ("troublemakers-log-test-" + std::to_string(unique));
    std::error_code ec;
    fs::create_directories(root, ec);
    if (check(!ec, "create temporary directory")) return 1;

    int result = 0;
    result |= check(start(root, "test-version"), "start first logger");
    std::printf("stdout-first-session\n");
    std::fprintf(stderr, "stderr-first-session\n");
    stop();

    const std::string first = read_file(root / "logs" / "latest.log");
    result |= check(first.find("Version: test-version") != std::string::npos,
                    "diagnostic header");
    result |= check(first.find("stdout-first-session") != std::string::npos,
                    "stdout capture");
    result |= check(first.find("stderr-first-session") != std::string::npos,
                    "stderr capture");

    result |= check(start(root, "test-version"), "start second logger");
    std::fprintf(stderr, "retained-static-diagnostic\n");
    const std::string noisy_chunk(64 * 1024, 'x');
    for (int i = 0; i < 10; ++i) {
        std::fwrite(noisy_chunk.data(), 1, noisy_chunk.size(), stdout);
    }
    std::fprintf(stderr, "\nsecond-session-tail\n");
    stop();

    const std::string previous = read_file(root / "logs" / "previous.log");
    const std::string current = read_file(root / "logs" / "latest.log");
    result |= check(previous.find("stdout-first-session") != std::string::npos,
                    "latest rotates to previous");
    result |= check(current.find("second-session-tail") != std::string::npos,
                    "new latest session");
    result |= check(current.find("retained-static-diagnostic") != std::string::npos,
                    "compaction preserves initial diagnostics");
    result |= check(current.find("[+") != std::string::npos,
                    "captured lines receive elapsed timestamps");
    result |= check(fs::file_size(root / "logs" / "latest.log") <= 512 * 1024,
                    "latest log is bounded");
    result |= check(current.find("older session output was discarded") !=
                        std::string::npos,
                    "compaction marker");
    result |= check(!read_report(Session::Previous, 4096).empty(),
                    "bounded support report");

    fs::remove_all(root, ec);
    return result == 0 ? 0 : 1;
}
