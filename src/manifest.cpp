#include "clipbank/manifest.hpp"

#include "clipbank/errors.hpp"
#include "clipbank/profile.hpp"
#include "clipbank/util.hpp"

#include <set>
#include <vector>

namespace cb {

nlohmann::json build_manifest(const LibraryPaths& paths, Db& db,
                              const Toolchain& toolchain, const std::string& run_filter) {
  std::vector<RunRow> runs;
  if (run_filter.empty()) {
    runs = db.list_done_runs();
  } else {
    const auto run = db.get_run(run_filter);
    if (!run) {
      throw UsageError("unknown run: " + run_filter);
    }
    if (run->state != "done") {
      throw UsageError("run is not done: " + run_filter + " (state=" + run->state + ")");
    }
    runs.push_back(*run);
  }

  Profile profile = Profile::detect();
  std::string profile_name = profile.name;
  std::string profile_fp = profile.fp();
  if (!runs.empty()) {
    profile_name = runs.front().profile;
    profile_fp = runs.front().profile_fp;
    if (profile_name != "detect") {
      throw UsageError("manifest v0.1.0 supports only the detect profile, found: " + profile_name);
    }
    for (const RunRow& run : runs) {
      if (run.profile_fp != profile_fp) {
        throw UsageError("manifest v1 supports a single profile; mixed profile_fp found (" +
                         runs.front().profile_fp + ", " + run.profile_fp + ")");
      }
    }
  }
  profile.name = profile_name;

  nlohmann::json sources = nlohmann::json::array();
  nlohmann::json clips = nlohmann::json::array();
  std::set<std::string> seen_sources;

  for (const RunRow& run : runs) {
    const auto source = db.get_source(run.source_id);
    if (!source) {
      throw DbError("source missing for run " + run.run_id + ": " + run.source_id);
    }
    if (seen_sources.insert(run.source_id).second) {
      sources.push_back({
          {"source_id", source->source_id},
          {"path", source->path},
          {"sha256", source->sha256},
          {"width", source->width},
          {"height", source->height},
          {"fps", source->avg.value()},
          {"duration_ms", source->duration_ms},
          {"vfr", source->vfr},
          {"vfr_ratio", source->vfr_ratio},
          {"hdr", source->hdr},
          {"rotation", source->rotation},
      });
    }

    for (const ClipRow& clip : db.get_clips(run.run_id)) {
      clips.push_back({
          {"clip_id", clip.clip_id},
          {"run_id", clip.run_id},
          {"source_id", run.source_id},
          {"path", clip.path},
          {"rel_path", relative_path_string(clip.path, paths.root)},
          {"start_ms", clip.start_ms},
          {"end_ms", clip.end_ms},
          {"frames", clip.frames},
          {"fps", static_cast<double>(profile.fps_num) / static_cast<double>(profile.fps_den)},
          {"width", profile.width},
          {"height", profile.height},
          {"sha256", clip.sha256.empty() ? nlohmann::json(nullptr) : nlohmann::json(clip.sha256)},
      });
    }
  }

  return nlohmann::json{
      {"schema", 1},
      {"generated_at", now_iso8601()},
      {"profile", profile_name},
      {"profile_fp", profile_fp},
      {"profile_params", profile.params()},
      {"toolchain", toolchain.to_json()},
      {"library", absolute_path(paths.root)},
      {"sources", std::move(sources)},
      {"clips", std::move(clips)},
  };
}

}  // namespace cb
