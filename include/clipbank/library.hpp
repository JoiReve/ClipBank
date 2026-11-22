#pragma once

#include "clipbank/util.hpp"

#include <string>

namespace cb {

struct LibraryPaths {
    fs::path root;
    fs::path db_path;
    fs::path tmp_dir;
    fs::path live_dir;

    fs::path run_dir(const std::string& run_id) const { return root / run_id; }
    fs::path clips_dir(const std::string& run_id) const { return run_dir(run_id) / "clips"; }
    fs::path normalized_path(const std::string& run_id) const {
        return run_dir(run_id) / "normalized.mp4";
    }
    fs::path thumb_path(const std::string& run_id) const { return run_dir(run_id) / "thumb.jpg"; }
    fs::path meta_path(const std::string& run_id) const { return run_dir(run_id) / "meta.json"; }
    fs::path log_path(const std::string& run_id) const { return run_dir(run_id) / "run.log"; }
    fs::path tmp_run_dir(const std::string& run_id) const { return tmp_dir / run_id; }
    fs::path tmp_clips_dir(const std::string& run_id) const {
        return tmp_run_dir(run_id) / "clips";
    }
};

// Resolves the user-supplied path (usually --library or $CLIPBANK_LIBRARY) to
// absolute paths. Does not create anything.
LibraryPaths resolve_library(const fs::path& root);

// Creates the directory layout and database schema. Idempotent.
LibraryPaths init_library(const fs::path& root);

// Validates that a library exists and is initialized.
LibraryPaths open_library(const fs::path& root);

} // namespace cb
