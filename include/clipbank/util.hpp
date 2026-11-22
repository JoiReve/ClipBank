#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace cb {

namespace fs = std::filesystem;

struct FileStat {
    int64_t size = 0;
    int64_t mtime_ns = 0;
};

FileStat stat_file(const fs::path& path);

int64_t unix_ms_now();
int64_t deadline_after_ms(int64_t milliseconds);
bool deadline_expired(int64_t deadline_ms);

// Local time in RFC 3339 form, e.g. 2026-11-20T10:00:00+08:00.
std::string now_iso8601();

std::string format_duration(int64_t milliseconds);
std::string human_bytes(int64_t bytes);

std::string quote_arg(const std::string& arg);
std::string join_command(const std::vector<std::string>& args);

std::string to_lower(std::string text);
bool ends_with_ci(const std::string& text, const std::string& suffix);
std::string trim(std::string text);

bool is_video_extension(const fs::path& path);

std::string read_file(const fs::path& path, std::size_t max_bytes = 16u * 1024u * 1024u);
void write_file_atomic(const fs::path& path, const std::string& content);
void ensure_dir(const fs::path& path);

std::string absolute_path(const fs::path& path);
std::string relative_path_string(const fs::path& path, const fs::path& base);

// Streams the file through SHA-256. A non-zero deadline (unix ms) aborts the
// read with InternalError when exceeded.
std::string sha256_file(const fs::path& path, int64_t deadline_ms = 0);

} // namespace cb
