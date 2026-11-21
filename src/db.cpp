#include "clipbank/db.hpp"

#include "clipbank/errors.hpp"

#include <sqlite3.h>

#include <utility>

namespace cb {
namespace {

class Statement {
 public:
  Statement(sqlite3* db, const std::string& sql) : db_(db) {
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt_, nullptr) != SQLITE_OK) {
      throw DbError(std::string("sqlite prepare failed: ") + sqlite3_errmsg(db_) + " | SQL: " + sql);
    }
  }

  ~Statement() {
    if (stmt_ != nullptr) {
      sqlite3_finalize(stmt_);
    }
  }

  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;

  Statement& bind(int index, int64_t value) {
    if (sqlite3_bind_int64(stmt_, index, value) != SQLITE_OK) {
      throw DbError(std::string("sqlite bind failed: ") + sqlite3_errmsg(db_));
    }
    return *this;
  }

  Statement& bind(int index, int value) { return bind(index, static_cast<int64_t>(value)); }

  Statement& bind(int index, double value) {
    if (sqlite3_bind_double(stmt_, index, value) != SQLITE_OK) {
      throw DbError(std::string("sqlite bind failed: ") + sqlite3_errmsg(db_));
    }
    return *this;
  }

  Statement& bind(int index, const std::string& value) {
    if (sqlite3_bind_text(stmt_, index, value.c_str(), static_cast<int>(value.size()),
                          SQLITE_TRANSIENT) != SQLITE_OK) {
      throw DbError(std::string("sqlite bind failed: ") + sqlite3_errmsg(db_));
    }
    return *this;
  }

  Statement& bind_null(int index) {
    if (sqlite3_bind_null(stmt_, index) != SQLITE_OK) {
      throw DbError(std::string("sqlite bind failed: ") + sqlite3_errmsg(db_));
    }
    return *this;
  }

  bool step() {
    const int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) {
      return true;
    }
    if (rc == SQLITE_DONE) {
      return false;
    }
    throw DbError(std::string("sqlite step failed: ") + sqlite3_errmsg(db_));
  }

  void execute() {
    if (step()) {
      throw DbError("sqlite statement unexpectedly returned rows");
    }
  }

  void reset() {
    sqlite3_reset(stmt_);
    sqlite3_clear_bindings(stmt_);
  }

  int64_t int64(int column) const { return sqlite3_column_int64(stmt_, column); }
  int integer(int column) const { return sqlite3_column_int(stmt_, column); }
  double real(int column) const { return sqlite3_column_double(stmt_, column); }

  std::string text(int column) const {
    const auto* value = sqlite3_column_text(stmt_, column);
    return value == nullptr ? std::string()
                            : std::string(reinterpret_cast<const char*>(value),
                                          static_cast<std::size_t>(sqlite3_column_bytes(stmt_, column)));
  }

 private:
  sqlite3* db_ = nullptr;
  sqlite3_stmt* stmt_ = nullptr;
};

SourceRow read_source(const Statement& stmt) {
  SourceRow row;
  row.source_id = stmt.text(0);
  row.path = stmt.text(1);
  row.size = stmt.int64(2);
  row.mtime_ns = stmt.int64(3);
  row.sha256 = stmt.text(4);
  row.width = stmt.integer(5);
  row.height = stmt.integer(6);
  row.fps = {stmt.int64(7), stmt.int64(8)};
  row.avg = {stmt.int64(9), stmt.int64(10)};
  row.duration_ms = stmt.int64(11);
  row.nb_frames = stmt.int64(12);
  row.vfr = stmt.integer(13) != 0;
  row.vfr_ratio = stmt.real(14);
  row.hdr = stmt.integer(15) != 0;
  row.rotation = stmt.integer(16);
  row.codec = stmt.text(17);
  row.created_at = stmt.text(18);
  return row;
}

RunRow read_run(const Statement& stmt, int offset) {
  RunRow row;
  row.run_id = stmt.text(offset + 0);
  row.source_id = stmt.text(offset + 1);
  row.profile = stmt.text(offset + 2);
  row.profile_fp = stmt.text(offset + 3);
  row.toolchain_fp = stmt.text(offset + 4);
  row.normalized_path = stmt.text(offset + 5);
  row.normalized_sha256 = stmt.text(offset + 6);
  row.duration_ms = stmt.int64(offset + 7);
  row.state = stmt.text(offset + 8);
  row.attempt = stmt.integer(offset + 9);
  row.error = stmt.text(offset + 10);
  row.created_at = stmt.text(offset + 11);
  row.updated_at = stmt.text(offset + 12);
  return row;
}

constexpr const char* kRunColumns =
    "r.run_id, r.source_id, r.profile, r.profile_fp, COALESCE(r.toolchain_fp,''), "
    "COALESCE(r.normalized_path,''), COALESCE(r.normalized_sha256,''), "
    "COALESCE(r.duration_ms,0), r.state, r.attempt, COALESCE(r.error,''), "
    "r.created_at, r.updated_at";

}  // namespace

Db::Db(const fs::path& path) {
  const int rc = sqlite3_open_v2(path.c_str(), &db_,
                                 SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                                 nullptr);
  if (rc != SQLITE_OK) {
    const std::string message = db_ != nullptr ? sqlite3_errmsg(db_) : "unknown sqlite error";
    if (db_ != nullptr) {
      sqlite3_close(db_);
      db_ = nullptr;
    }
    throw DbError("cannot open database " + path.string() + ": " + message);
  }
  sqlite3_busy_timeout(db_, 5000);
  exec("PRAGMA foreign_keys = ON;");
  exec("PRAGMA synchronous = NORMAL;");
}

Db::~Db() {
  if (db_ != nullptr) {
    sqlite3_close(db_);
  }
}

void Db::exec(const std::string& sql) const {
  char* error = nullptr;
  if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
    const std::string message = error != nullptr ? error : sqlite3_errmsg(db_);
    sqlite3_free(error);
    throw DbError("sqlite exec failed: " + message);
  }
}

void Db::ensure_schema() {
  exec("PRAGMA journal_mode = WAL;");
  exec(R"SQL(
    CREATE TABLE IF NOT EXISTS sources (
      source_id   TEXT PRIMARY KEY,
      path        TEXT NOT NULL,
      size        INTEGER NOT NULL,
      mtime_ns    INTEGER NOT NULL,
      sha256      TEXT NOT NULL,
      width       INTEGER,
      height      INTEGER,
      fps_num     INTEGER,
      fps_den     INTEGER,
      avg_num     INTEGER,
      avg_den     INTEGER,
      duration_ms INTEGER,
      nb_frames   INTEGER,
      vfr         INTEGER NOT NULL DEFAULT 0,
      vfr_ratio   REAL    NOT NULL DEFAULT 0,
      hdr         INTEGER NOT NULL DEFAULT 0,
      rotation    INTEGER NOT NULL DEFAULT 0,
      codec       TEXT,
      created_at  TEXT NOT NULL
    );

    CREATE TABLE IF NOT EXISTS runs (
      run_id            TEXT PRIMARY KEY,
      source_id         TEXT NOT NULL REFERENCES sources(source_id),
      profile           TEXT NOT NULL,
      profile_fp        TEXT NOT NULL,
      toolchain_fp      TEXT,
      normalized_path   TEXT,
      normalized_sha256 TEXT,
      duration_ms       INTEGER,
      state             TEXT NOT NULL,
      attempt           INTEGER NOT NULL DEFAULT 0,
      error             TEXT,
      created_at        TEXT NOT NULL,
      updated_at        TEXT NOT NULL,
      UNIQUE(source_id, profile_fp)
    );

    CREATE TABLE IF NOT EXISTS clips (
      clip_id   TEXT PRIMARY KEY,
      run_id    TEXT NOT NULL REFERENCES runs(run_id),
      idx       INTEGER NOT NULL,
      path      TEXT NOT NULL,
      start_ms  INTEGER NOT NULL,
      end_ms    INTEGER NOT NULL,
      frames    INTEGER NOT NULL,
      sha256    TEXT,
      UNIQUE(run_id, idx)
    );

    CREATE INDEX IF NOT EXISTS idx_runs_state ON runs(state);
    CREATE INDEX IF NOT EXISTS idx_clips_run  ON clips(run_id);
    PRAGMA user_version = 1;
  )SQL");
}

void Db::upsert_source(const SourceRow& source) {
  Statement stmt(db_, R"SQL(
    INSERT INTO sources (
      source_id, path, size, mtime_ns, sha256, width, height,
      fps_num, fps_den, avg_num, avg_den, duration_ms, nb_frames,
      vfr, vfr_ratio, hdr, rotation, codec, created_at
    ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    ON CONFLICT(source_id) DO UPDATE SET
      path=excluded.path, size=excluded.size, mtime_ns=excluded.mtime_ns,
      sha256=excluded.sha256, width=excluded.width, height=excluded.height,
      fps_num=excluded.fps_num, fps_den=excluded.fps_den,
      avg_num=excluded.avg_num, avg_den=excluded.avg_den,
      duration_ms=excluded.duration_ms, nb_frames=excluded.nb_frames,
      vfr=excluded.vfr, vfr_ratio=excluded.vfr_ratio, hdr=excluded.hdr,
      rotation=excluded.rotation, codec=excluded.codec
  )SQL");
  stmt.bind(1, source.source_id)
      .bind(2, source.path)
      .bind(3, source.size)
      .bind(4, source.mtime_ns)
      .bind(5, source.sha256)
      .bind(6, source.width)
      .bind(7, source.height)
      .bind(8, source.fps.num)
      .bind(9, source.fps.den)
      .bind(10, source.avg.num)
      .bind(11, source.avg.den)
      .bind(12, source.duration_ms)
      .bind(13, source.nb_frames)
      .bind(14, static_cast<int>(source.vfr))
      .bind(15, source.vfr_ratio)
      .bind(16, static_cast<int>(source.hdr))
      .bind(17, source.rotation)
      .bind(18, source.codec)
      .bind(19, source.created_at.empty() ? now_iso8601() : source.created_at);
  stmt.execute();
}

std::optional<SourceRow> Db::get_source(const std::string& source_id) const {
  Statement stmt(db_, R"SQL(
    SELECT source_id, path, size, mtime_ns, sha256, width, height,
           fps_num, fps_den, avg_num, avg_den, duration_ms, nb_frames,
           vfr, vfr_ratio, hdr, rotation, COALESCE(codec,''), created_at
    FROM sources WHERE source_id = ?
  )SQL");
  stmt.bind(1, source_id);
  if (!stmt.step()) {
    return std::nullopt;
  }
  return read_source(stmt);
}

std::optional<RunRow> Db::find_run(const std::string& source_id,
                                   const std::string& profile_fp) const {
  Statement stmt(db_, std::string("SELECT ") + kRunColumns +
                          " FROM runs r WHERE r.source_id = ? AND r.profile_fp = ?");
  stmt.bind(1, source_id).bind(2, profile_fp);
  if (!stmt.step()) {
    return std::nullopt;
  }
  return read_run(stmt, 0);
}

std::optional<RunRow> Db::get_run(const std::string& run_id) const {
  Statement stmt(db_, std::string("SELECT ") + kRunColumns + " FROM runs r WHERE r.run_id = ?");
  stmt.bind(1, run_id);
  if (!stmt.step()) {
    return std::nullopt;
  }
  return read_run(stmt, 0);
}

void Db::insert_run(const RunRow& run) {
  const std::string now = now_iso8601();
  Statement stmt(db_, R"SQL(
    INSERT INTO runs (
      run_id, source_id, profile, profile_fp, toolchain_fp,
      normalized_path, normalized_sha256, duration_ms, state, attempt,
      error, created_at, updated_at
    ) VALUES (?, ?, ?, ?, ?, NULL, NULL, 0, ?, ?, NULL, ?, ?)
  )SQL");
  stmt.bind(1, run.run_id)
      .bind(2, run.source_id)
      .bind(3, run.profile)
      .bind(4, run.profile_fp)
      .bind(5, run.toolchain_fp)
      .bind(6, run.state.empty() ? "queued" : run.state)
      .bind(7, run.attempt)
      .bind(8, now)
      .bind(9, now);
  stmt.execute();
}

void Db::bump_attempt(const std::string& run_id) {
  Statement stmt(db_, "UPDATE runs SET attempt = attempt + 1, updated_at = ? WHERE run_id = ?");
  stmt.bind(1, now_iso8601()).bind(2, run_id);
  stmt.execute();
}

void Db::set_state(const std::string& run_id, const std::string& state,
                   const std::optional<std::string>& error) {
  Statement stmt(db_, "UPDATE runs SET state = ?, error = ?, updated_at = ? WHERE run_id = ?");
  stmt.bind(1, state);
  if (error) {
    stmt.bind(2, *error);
  } else {
    stmt.bind_null(2);
  }
  stmt.bind(3, now_iso8601()).bind(4, run_id);
  stmt.execute();
}

void Db::set_toolchain(const std::string& run_id, const std::string& toolchain_fp) {
  Statement stmt(db_, "UPDATE runs SET toolchain_fp = ?, updated_at = ? WHERE run_id = ?");
  stmt.bind(1, toolchain_fp).bind(2, now_iso8601()).bind(3, run_id);
  stmt.execute();
}

void Db::set_normalized(const std::string& run_id, const std::string& path,
                        const std::string& sha256, int64_t duration_ms) {
  Statement stmt(db_,
                 "UPDATE runs SET normalized_path = ?, normalized_sha256 = ?, duration_ms = ?, "
                 "updated_at = ? WHERE run_id = ?");
  stmt.bind(1, path);
  if (sha256.empty()) {
    stmt.bind_null(2);
  } else {
    stmt.bind(2, sha256);
  }
  stmt.bind(3, duration_ms).bind(4, now_iso8601()).bind(5, run_id);
  stmt.execute();
}

void Db::set_normalized_sha(const std::string& run_id, const std::string& sha256) {
  Statement stmt(db_, "UPDATE runs SET normalized_sha256 = ?, updated_at = ? WHERE run_id = ?");
  stmt.bind(1, sha256).bind(2, now_iso8601()).bind(3, run_id);
  stmt.execute();
}

void Db::set_duration(const std::string& run_id, int64_t duration_ms) {
  Statement stmt(db_, "UPDATE runs SET duration_ms = ?, updated_at = ? WHERE run_id = ?");
  stmt.bind(1, duration_ms).bind(2, now_iso8601()).bind(3, run_id);
  stmt.execute();
}

void Db::clear_clips(const std::string& run_id) {
  Statement stmt(db_, "DELETE FROM clips WHERE run_id = ?");
  stmt.bind(1, run_id);
  stmt.execute();
}

void Db::replace_clips(const std::string& run_id, const std::vector<ClipRow>& clips) {
  exec("BEGIN IMMEDIATE;");
  try {
    clear_clips(run_id);
    Statement stmt(db_, R"SQL(
      INSERT INTO clips (clip_id, run_id, idx, path, start_ms, end_ms, frames, sha256)
      VALUES (?, ?, ?, ?, ?, ?, ?, ?)
    )SQL");
    for (const ClipRow& clip : clips) {
      stmt.reset();
      stmt.bind(1, clip.clip_id)
          .bind(2, clip.run_id)
          .bind(3, clip.idx)
          .bind(4, clip.path)
          .bind(5, clip.start_ms)
          .bind(6, clip.end_ms)
          .bind(7, clip.frames);
      if (clip.sha256.empty()) {
        stmt.bind_null(8);
      } else {
        stmt.bind(8, clip.sha256);
      }
      stmt.execute();
    }
    exec("COMMIT;");
  } catch (...) {
    try {
      exec("ROLLBACK;");
    } catch (...) {
    }
    throw;
  }
}

std::vector<ClipRow> Db::get_clips(const std::string& run_id) const {
  Statement stmt(db_, R"SQL(
    SELECT clip_id, run_id, idx, path, start_ms, end_ms, frames, COALESCE(sha256,'')
    FROM clips WHERE run_id = ? ORDER BY idx
  )SQL");
  stmt.bind(1, run_id);
  std::vector<ClipRow> clips;
  while (stmt.step()) {
    ClipRow clip;
    clip.clip_id = stmt.text(0);
    clip.run_id = stmt.text(1);
    clip.idx = stmt.int64(2);
    clip.path = stmt.text(3);
    clip.start_ms = stmt.int64(4);
    clip.end_ms = stmt.int64(5);
    clip.frames = stmt.int64(6);
    clip.sha256 = stmt.text(7);
    clips.push_back(std::move(clip));
  }
  return clips;
}

int64_t Db::count_clips(const std::string& run_id) const {
  Statement stmt(db_, "SELECT COUNT(*) FROM clips WHERE run_id = ?");
  stmt.bind(1, run_id);
  stmt.step();
  return stmt.int64(0);
}

void Db::set_clip_hash(const std::string& clip_id, const std::string& sha256) {
  Statement stmt(db_, "UPDATE clips SET sha256 = ? WHERE clip_id = ?");
  stmt.bind(1, sha256).bind(2, clip_id);
  stmt.execute();
}

std::vector<RunSummary> Db::list_runs(const std::string& state_filter,
                                      const std::string& profile_filter) const {
  std::string sql = std::string("SELECT ") + kRunColumns +
                    ", s.path, (SELECT COUNT(*) FROM clips c WHERE c.run_id = r.run_id) "
                    "FROM runs r JOIN sources s ON s.source_id = r.source_id";
  if (!state_filter.empty() && !profile_filter.empty()) {
    sql += " WHERE r.state = ? AND r.profile = ?";
  } else if (!state_filter.empty()) {
    sql += " WHERE r.state = ?";
  } else if (!profile_filter.empty()) {
    sql += " WHERE r.profile = ?";
  }
  sql += " ORDER BY r.created_at, r.run_id";

  Statement stmt(db_, sql);
  int bind_index = 1;
  if (!state_filter.empty()) {
    stmt.bind(bind_index++, state_filter);
  }
  if (!profile_filter.empty()) {
    stmt.bind(bind_index++, profile_filter);
  }

  std::vector<RunSummary> rows;
  while (stmt.step()) {
    RunSummary summary;
    summary.run = read_run(stmt, 0);
    summary.source_path = stmt.text(13);
    summary.clip_count = stmt.int64(14);
    rows.push_back(std::move(summary));
  }
  return rows;
}

std::vector<RunRow> Db::list_done_runs() const {
  Statement stmt(db_, std::string("SELECT ") + kRunColumns +
                          " FROM runs r WHERE r.state = 'done' ORDER BY r.created_at, r.run_id");
  std::vector<RunRow> rows;
  while (stmt.step()) {
    rows.push_back(read_run(stmt, 0));
  }
  return rows;
}

std::vector<RunRow> Db::list_incomplete_runs() const {
  Statement stmt(db_, std::string("SELECT ") + kRunColumns +
                          " FROM runs r WHERE r.state != 'done' ORDER BY r.created_at, r.run_id");
  std::vector<RunRow> rows;
  while (stmt.step()) {
    rows.push_back(read_run(stmt, 0));
  }
  return rows;
}

void Db::reset_run_for_reencode(const std::string& run_id) {
  Statement stmt(db_, R"SQL(
    UPDATE runs SET normalized_path = NULL, normalized_sha256 = NULL,
                    duration_ms = 0, state = 'queued', error = NULL, updated_at = ?
    WHERE run_id = ?
  )SQL");
  stmt.bind(1, now_iso8601()).bind(2, run_id);
  stmt.execute();
  clear_clips(run_id);
}

}  // namespace cb
