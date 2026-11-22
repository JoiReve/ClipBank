#pragma once

#include "clipbank/db.hpp"
#include "clipbank/library.hpp"

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace cb {

struct VerifyOptions {
    std::string run_id; // empty = all done runs
    bool no_hash = false;
};

struct VerifyRunResult {
    std::string run_id;
    std::string source_path;
    bool ok = true;
    int64_t clip_count = 0;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

struct VerifySummary {
    bool ok = true;
    std::vector<VerifyRunResult> runs;
};

VerifySummary verify_library(const LibraryPaths& paths, Db& db, const VerifyOptions& options);

nlohmann::json verify_to_json(const VerifySummary& summary);

// Shared tolerance helpers (also covered by unit tests).
int64_t normalized_duration_tolerance_ms(int64_t reference_ms);
int64_t clip_duration_tolerance_ms();
bool frames_close(int64_t actual, int64_t expected);

} // namespace cb
