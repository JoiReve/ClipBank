#pragma once

#include "clipbank/db.hpp"
#include "clipbank/library.hpp"
#include "clipbank/probe.hpp"

#include <string>

#include <nlohmann/json.hpp>

namespace cb {

// Builds manifest schema v1. An empty run_filter covers every done run.
// Throws UsageError if run_filter names an unknown or unfinished run.
nlohmann::json build_manifest(const LibraryPaths& paths, Db& db,
                              const Toolchain& toolchain,
                              const std::string& run_filter = std::string());

}  // namespace cb
