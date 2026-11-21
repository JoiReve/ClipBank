#pragma once

#include "clipbank/types.hpp"
#include "clipbank/util.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace cb {

struct SourceRow {
  std::string source_id;
  std::string path;
  int64_t size = 0;
  int64_t mtime_ns = 0;
  std::string sha256;

  int width = 0;
  int height = 0;
  Rational fps;
  Rational avg;
  int64_t duration_ms = 0;
  int64_t nb_frames = 0;

  bool vfr = false;
  double vfr_ratio = 0.0;
  bool hdr = false;
  int rotation = 0;
  std::string codec;
  std::string created_at;
};

struct RunRow {
  std::string run_id;
  std::string source_id;
  std::string profile;
  std::string profile_fp;
  std::string toolchain_fp;
  std::string normalized_path;
  std::string normalized_sha256;
  int64_t duration_ms = 0;
  std::string state;
  int attempt = 0;
  std::string error;
  std::string created_at;
  std::string updated_at;
};

struct ClipRow {
  std::string clip_id;
  std::string run_id;
  int64_t idx = 0;
  std::string path;
  int64_t start_ms = 0;
  int64_t end_ms = 0;
  int64_t frames = 0;
  std::string sha256;
};

struct RunSummary {
  RunRow run;
  std::string source_path;
  int64_t clip_count = 0;
};

class Db {
 public:
  explicit Db(const fs::path& path);
  ~Db();

  Db(const Db&) = delete;
  Db& operator=(const Db&) = delete;

  void ensure_schema();

  void upsert_source(const SourceRow& source);
  std::optional<SourceRow> get_source(const std::string& source_id) const;

  std::optional<RunRow> find_run(const std::string& source_id, const std::string& profile_fp) const;
  std::optional<RunRow> get_run(const std::string& run_id) const;
  void insert_run(const RunRow& run);

  void bump_attempt(const std::string& run_id);
  void set_state(const std::string& run_id, const std::string& state,
                 const std::optional<std::string>& error);
  void set_toolchain(const std::string& run_id, const std::string& toolchain_fp);
  void set_normalized(const std::string& run_id, const std::string& path,
                      const std::string& sha256, int64_t duration_ms);
  void set_normalized_sha(const std::string& run_id, const std::string& sha256);
  void set_duration(const std::string& run_id, int64_t duration_ms);

  void clear_clips(const std::string& run_id);
  void replace_clips(const std::string& run_id, const std::vector<ClipRow>& clips);
  std::vector<ClipRow> get_clips(const std::string& run_id) const;
  int64_t count_clips(const std::string& run_id) const;
  void set_clip_hash(const std::string& clip_id, const std::string& sha256);

  std::vector<RunSummary> list_runs(const std::string& state_filter = "",
                                    const std::string& profile_filter = "") const;
  std::vector<RunRow> list_done_runs() const;
  std::vector<RunRow> list_incomplete_runs() const;

  void reset_run_for_reencode(const std::string& run_id);

 private:
  void exec(const std::string& sql) const;

  sqlite3* db_ = nullptr;
};

}  // namespace cb
