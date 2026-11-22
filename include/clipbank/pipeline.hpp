#pragma once

#include "clipbank/db.hpp"
#include "clipbank/library.hpp"
#include "clipbank/probe.hpp"
#include "clipbank/profile.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace cb {

struct PipelineOptions {
    bool dry_run = false;
    bool no_hash = false;
    bool force_reencode = false;
    bool verbose = false;
};

// clang-format off
enum class OutcomeKind { Done, Skipped, Failed, DryRun };
// clang-format on

struct ProcessOutcome {
    OutcomeKind kind = OutcomeKind::Failed;
    std::string run_id;
    std::string message;
    int exit_code = 0;
};

struct ResumePlan {
    bool normalized_ok = false;
    bool clips_ok = false;
    bool hashes_ok = false;
};

ResumePlan decide_resume(bool normalized_exists, bool clip_rows_present,
                         bool clip_files_present, bool hashes_complete, bool no_hash);

int64_t timeout_for_normalize_ms(int64_t duration_ms);
int64_t timeout_for_segment_ms(int64_t duration_ms);
int64_t timeout_for_hash_ms(int64_t file_size_bytes);

// Parses one line of `ffmpeg -progress pipe:1` output. out_time_ms is
// deliberately ignored: in ffmpeg 8 it is microseconds, not milliseconds.
bool parse_ffmpeg_progress_line(const std::string& line, int64_t& out_time_us, bool& ended);

// Durations in -> [start_ms, end_ms) ranges accumulated from real segment
// durations. Returns starts/ends; the number of entries is durations.size().
void accumulate_clip_ranges(const std::vector<int64_t>& durations,
                            std::vector<int64_t>& starts, std::vector<int64_t>& ends);

class Pipeline {
public:
    Pipeline(const LibraryPaths& paths, Db* db, Profile profile, Toolchain toolchain,
             PipelineOptions options);

    // Hashes, probes and (unless dry_run) runs the full pipeline. If retry_mode
    // is set, expected_source_id must match the current file content.
    ProcessOutcome process_path(const fs::path& input, bool retry_mode = false,
                                const std::string& expected_source_id = std::string());

private:
    void start_log(const std::string& run_id);
    void stop_log();
    void emit(const std::string& message, bool console = true);
    void console(const std::string& message) const;

    void cleanup_derived(const std::string& run_id);
    bool file_ok(const fs::path& path) const;

    std::string stage_ = "queued";

    void run_normalize(const std::string& run_id, const fs::path& source, const ProbeInfo& source_info);
    void run_segment(const std::string& run_id);
    void run_thumb(const std::string& run_id);
    void run_hash(const std::string& run_id);
    void write_meta(const std::string& run_id, const ProbeInfo& source_info);

    std::vector<std::string> normalize_command(const fs::path& source, const fs::path& output) const;
    std::vector<std::string> segment_command(const fs::path& input, const fs::path& pattern) const;
    std::vector<std::string> thumb_command(const fs::path& input, const fs::path& output,
                                           int64_t duration_ms) const;

    LibraryPaths paths_;
    Db* db_ = nullptr;
    Profile profile_;
    Toolchain toolchain_;
    PipelineOptions options_;
    std::optional<std::ofstream> log_;
};

} // namespace cb
