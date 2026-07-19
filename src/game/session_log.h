// Persistent, bounded session diagnostics. stdout and stderr are tee'd into a
// rotating log so diagnostics emitted by the game, runtime, SDL, and RT64 all
// arrive in one support report.
#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

namespace mm::session_log {

enum class Session {
    Current,
    Previous,
};

bool start(const std::filesystem::path& app_folder, const std::string& version);
void flush();
void stop();

std::filesystem::path log_directory();
std::filesystem::path log_path(Session session);
bool has_report(Session session);

// Preserve the diagnostic header and newest output while keeping the result
// small enough to paste into a GitHub issue.
std::string read_report(Session session, size_t max_bytes = 48 * 1024);

} // namespace mm::session_log
