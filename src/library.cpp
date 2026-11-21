#include "clipbank/library.hpp"

#include "clipbank/db.hpp"
#include "clipbank/errors.hpp"

namespace cb {

LibraryPaths resolve_library(const fs::path& root) {
  LibraryPaths paths;
  paths.root = absolute_path(root);
  paths.db_path = paths.root / "clipbank.db";
  paths.tmp_dir = paths.root / ".tmp";
  paths.live_dir = paths.root / "live";
  return paths;
}

LibraryPaths init_library(const fs::path& root) {
  LibraryPaths paths = resolve_library(root);
  ensure_dir(paths.root);
  ensure_dir(paths.tmp_dir);
  ensure_dir(paths.live_dir);

  Db db(paths.db_path);
  db.ensure_schema();
  return paths;
}

LibraryPaths open_library(const fs::path& root) {
  LibraryPaths paths = resolve_library(root);
  std::error_code ec;
  if (!fs::is_regular_file(paths.db_path, ec)) {
    throw UsageError("library is not initialized at " + paths.root.string() +
                     " (run `clipbank init --library " + paths.root.string() + "` first)");
  }
  if (!fs::is_directory(paths.tmp_dir, ec)) {
    ensure_dir(paths.tmp_dir);
  }
  return paths;
}

}  // namespace cb
