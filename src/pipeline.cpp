#include "clipbank/pipeline.hpp"

#include "clipbank/errors.hpp"
#include "clipbank/subprocess.hpp"
#include "clipbank/util.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace cb {
namespace {

constexpr int64_t kProbeTimeoutMs = 60000;
constexpr int64_t kThumbTimeoutMs = 120000;

std::string format_seconds(double seconds) {
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(3) << seconds;
  return oss.str();
}

bool parse_timestamp_us(const std::string& text, int64_t& out_us) {
  const std::size_t first = text.find(':');
  const std::size_t second = first == std::string::npos ? std::string::npos : text.find(':', first + 1);
  if (first == std::string::npos || second == std::string::npos) {
    return false;
  }
  try {
    const int64_t hours = std::stoll(text.substr(0, first));
    const int64_t minutes = std::stoll(text.substr(first + 1, second - first - 1));
    const double seconds = std::stod(text.substr(second + 1));
    if (hours < 0 || minutes < 0 || seconds < 0.0) {
      return false;
    }
    out_us = hours * 3600LL * 1000000LL + minutes * 60LL * 1000000LL +
             static_cast<int64_t>(std::llround(seconds * 1000000.0));
    return true;
  } catch (...) {
    return false;
  }
}

bool starts_with(const std::string& text, const std::string& prefix) {
  return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

void remove_all_quiet(const fs::path& path) {
  std::error_code ec;
  fs::remove_all(path, ec);
}

void remove_all_checked(const fs::path& path) {
  std::error_code ec;
  fs::remove_all(path, ec);
  if (ec) {
    throw InternalError("cannot remove " + path.string() + ": " + ec.message());
  }
}

bool is_clip_filename(const fs::path& path) {
  if (path.extension() != ".mp4") {
    return false;
  }
  const std::string stem = path.stem().string();
  return stem.size() >= 4 && std::all_of(stem.begin(), stem.end(),
                                         [](unsigned char c) { return c >= '0' && c <= '9'; });
}

int64_t clip_number(const fs::path& path) {
  try {
    return std::stoll(path.stem().string());
  } catch (...) {
    return -1;
  }
}

std::vector<fs::path> list_clip_files(const fs::path& dir) {
  std::vector<fs::path> files;
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    if (it->is_regular_file(ec) && is_clip_filename(it->path())) {
      files.push_back(it->path());
    }
  }
  std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) {
    return clip_number(a) < clip_number(b);
  });
  return files;
}

std::string profile_fps_string(const Profile& profile) {
  if (profile.fps_den == 1) {
    return std::to_string(profile.fps_num);
  }
  return std::to_string(profile.fps_num) + "/" + std::to_string(profile.fps_den);
}

}  // namespace

ResumePlan decide_resume(bool normalized_exists, bool clip_rows_present,
                         bool clip_files_present, bool hashes_complete, bool no_hash) {
  ResumePlan plan;
  plan.normalized_ok = normalized_exists;
  plan.clips_ok = normalized_exists && clip_rows_present && clip_files_present;
  plan.hashes_ok = no_hash || hashes_complete;
  return plan;
}

int64_t timeout_for_normalize_ms(int64_t duration_ms) {
  return std::max<int64_t>(600000, duration_ms * 6);
}

int64_t timeout_for_segment_ms(int64_t duration_ms) {
  return std::max<int64_t>(300000, duration_ms * 2);
}

int64_t timeout_for_hash_ms(int64_t file_size_bytes) {
  constexpr double kBytesPerSecond = 50.0 * 1024.0 * 1024.0;
  const double seconds = static_cast<double>(std::max<int64_t>(0, file_size_bytes)) / kBytesPerSecond;
  return std::max<int64_t>(300000, static_cast<int64_t>(std::ceil(seconds * 1000.0)) + 60000);
}

bool parse_ffmpeg_progress_line(const std::string& line, int64_t& out_time_us, bool& ended) {
  ended = false;
  if (starts_with(line, "out_time_us=")) {
    try {
      out_time_us = std::stoll(line.substr(12));
      return true;
    } catch (...) {
      return false;
    }
  }
  if (starts_with(line, "out_time=")) {
    return parse_timestamp_us(line.substr(9), out_time_us);
  }
  if (starts_with(line, "progress=")) {
    ended = line.substr(9) == "end";
    return true;
  }
  // out_time_ms is intentionally not interpreted: ffmpeg 8 reports microseconds.
  return false;
}

void accumulate_clip_ranges(const std::vector<int64_t>& durations,
                            std::vector<int64_t>& starts, std::vector<int64_t>& ends) {
  starts.clear();
  ends.clear();
  starts.reserve(durations.size());
  ends.reserve(durations.size());
  int64_t offset = 0;
  for (const int64_t duration : durations) {
    starts.push_back(offset);
    offset += duration;
    ends.push_back(offset);
  }
}

Pipeline::Pipeline(const LibraryPaths& paths, Db* db, Profile profile, Toolchain toolchain,
                   PipelineOptions options)
    : paths_(paths),
      db_(db),
      profile_(std::move(profile)),
      toolchain_(std::move(toolchain)),
      options_(options) {}

void Pipeline::start_log(const std::string& run_id) {
  ensure_dir(paths_.run_dir(run_id));
  log_.emplace(paths_.log_path(run_id), std::ios::app);
  if (!*log_) {
    throw InternalError("cannot open run log " + paths_.log_path(run_id).string());
  }
}

void Pipeline::stop_log() {
  if (log_) {
    log_->flush();
    log_.reset();
  }
}

void Pipeline::emit(const std::string& message, bool console_output) {
  if (log_ && log_->is_open()) {
    *log_ << "[" << now_iso8601() << "] " << message << "\n";
    log_->flush();
  }
  if (console_output) {
    std::cout << message << "\n";
    std::cout.flush();
  }
}

void Pipeline::console(const std::string& message) const {
  std::cout << message << "\n";
  std::cout.flush();
}

bool Pipeline::file_ok(const fs::path& path) const {
  std::error_code ec;
  if (!fs::is_regular_file(path, ec)) {
    return false;
  }
  const auto size = fs::file_size(path, ec);
  return !ec && size > 0;
}

void Pipeline::cleanup_derived(const std::string& run_id) {
  remove_all_quiet(paths_.tmp_run_dir(run_id));
  std::error_code ec;
  fs::remove(paths_.normalized_path(run_id), ec);
  fs::remove_all(paths_.clips_dir(run_id), ec);
  fs::remove(paths_.thumb_path(run_id), ec);
  fs::remove(paths_.meta_path(run_id), ec);
  db_->clear_clips(run_id);
  db_->reset_run_for_reencode(run_id);
}

std::vector<std::string> Pipeline::normalize_command(const fs::path& source,
                                                     const fs::path& output) const {
  const std::string fps = profile_fps_string(profile_);
  std::ostringstream filter;
  filter << "fps=" << fps << ",scale=" << profile_.width << ":" << profile_.height
         << ":force_original_aspect_ratio=decrease,pad=" << profile_.width << ":"
         << profile_.height << ":(ow-iw)/2:(oh-ih)/2:color=black,setsar=1,format="
         << profile_.pix_fmt;

  return {ffmpeg_binary(),
          "-hide_banner",
          "-nostdin",
          "-y",
          "-loglevel",
          "error",
          "-i",
          source.string(),
          "-map",
          "0:V:0",
          "-an",
          "-sn",
          "-dn",
          "-map_metadata",
          "-1",
          "-vf",
          filter.str(),
          "-c:v",
          "libx264",
          "-preset",
          profile_.preset,
          "-crf",
          std::to_string(profile_.crf),
          "-profile:v",
          "high",
          "-force_key_frames",
          "expr:gte(t,n_forced*" + std::to_string(profile_.segment_seconds) + ")",
          "-movflags",
          "+faststart",
          "-metadata:s:v:0",
          "rotate=0",
          "-progress",
          "pipe:1",
          "-nostats",
          output.string()};
}

std::vector<std::string> Pipeline::segment_command(const fs::path& input,
                                                   const fs::path& pattern) const {
  return {ffmpeg_binary(),
          "-hide_banner",
          "-nostdin",
          "-y",
          "-loglevel",
          "error",
          "-i",
          input.string(),
          "-map",
          "0:v:0",
          "-c",
          "copy",
          "-f",
          "segment",
          "-segment_time",
          std::to_string(profile_.segment_seconds),
          "-segment_time_delta",
          "0.1",
          "-reset_timestamps",
          "1",
          "-segment_format",
          "mp4",
          pattern.string()};
}

std::vector<std::string> Pipeline::thumb_command(const fs::path& input, const fs::path& output,
                                                 int64_t duration_ms) const {
  const double seek_seconds = static_cast<double>(duration_ms) * 0.10 / 1000.0;
  return {ffmpeg_binary(), "-hide_banner", "-nostdin", "-y", "-loglevel", "error",
          "-ss", format_seconds(seek_seconds), "-i", input.string(),
          "-frames:v", "1", "-vf", "scale=320:-2", output.string()};
}

void Pipeline::run_normalize(const std::string& run_id, const fs::path& source,
                             const ProbeInfo& source_info) {
  const fs::path dest = paths_.normalized_path(run_id);
  const fs::path tmp_dir = paths_.tmp_run_dir(run_id);
  const fs::path tmp = tmp_dir / "normalized.mp4";
  remove_all_checked(tmp_dir);
  ensure_dir(tmp_dir);

  const auto args = normalize_command(source, tmp);
  emit("normalize: " + std::to_string(source_info.width) + "x" + std::to_string(source_info.height) +
       " -> " + std::to_string(profile_.width) + "x" + std::to_string(profile_.height) + " @" +
       profile_fps_string(profile_) + " fps");

  int64_t total_us = source_info.duration_ms * 1000;
  int last_pct = -10;

  ProcessOptions process;
  process.args = args;
  process.timeout = std::chrono::milliseconds(timeout_for_normalize_ms(source_info.duration_ms));
  process.stderr_tail_limit = 8192;
  process.on_stdout_line = [&](const std::string& line) {
    int64_t out_us = 0;
    bool ended = false;
    if (!parse_ffmpeg_progress_line(line, out_us, ended)) {
      return;
    }
    if (ended) {
      last_pct = 100;
      return;
    }
    if (total_us > 0) {
      int pct = static_cast<int>((out_us * 100) / total_us);
      pct = std::clamp(pct, 0, 100);
      if (pct >= last_pct + 10) {
        last_pct = pct;
        emit("  normalize " + std::to_string(pct) + "%", false);
      }
    }
  };

  const ProcessResult result = run_process(process);
  if (!result.started) {
    throw DependencyError("cannot start ffmpeg: " + result.error);
  }
  if (result.timed_out) {
    throw InternalError("normalize timed out after " + std::to_string(process.timeout.count()) + "ms");
  }
  if (!result.ok()) {
    throw InputError("normalize failed: " + trim(result.stderr_tail));
  }
  if (!file_ok(tmp)) {
    throw InternalError("normalize produced no output");
  }

  ProbeInfo normalized = probe_media(tmp, std::chrono::milliseconds(kProbeTimeoutMs));
  if (normalized.width != profile_.width || normalized.height != profile_.height) {
    throw InternalError("normalized dimensions are " + std::to_string(normalized.width) + "x" +
                        std::to_string(normalized.height));
  }
  const double expected_fps = static_cast<double>(profile_.fps_num) /
                              static_cast<double>(profile_.fps_den);
  if (!normalized.fps.valid() || std::abs(normalized.fps.value() - expected_fps) > 0.001) {
    throw InternalError("normalized frame rate is " + std::to_string(normalized.fps.num) + "/" +
                        std::to_string(normalized.fps.den));
  }
  if (normalized.pix_fmt != profile_.pix_fmt) {
    throw InternalError("normalized pixel format is " + normalized.pix_fmt);
  }
  if (normalized.has_audio()) {
    throw InternalError("normalized output still has audio");
  }
  if (normalized.duration_ms <= 0) {
    throw InternalError("normalized duration is zero");
  }

  std::error_code ec;
  fs::remove(dest, ec);
  fs::rename(tmp, dest, ec);
  if (ec) {
    throw InternalError("cannot publish normalized file: " + ec.message());
  }

  db_->set_normalized(run_id, absolute_path(dest), "", normalized.duration_ms);
  emit("normalize: ok (" + format_duration(normalized.duration_ms) + ")");
}

void Pipeline::run_segment(const std::string& run_id) {
  const fs::path input = paths_.normalized_path(run_id);
  if (!file_ok(input)) {
    throw InternalError("normalized file is missing before segment");
  }

  const fs::path tmp_dir = paths_.tmp_run_dir(run_id);
  const fs::path tmp_clips = paths_.tmp_clips_dir(run_id);
  remove_all_checked(tmp_dir);
  ensure_dir(tmp_clips);

  const fs::path pattern = tmp_clips / "%04d.mp4";
  const auto args = segment_command(input, pattern);

  const auto run = db_->get_run(run_id);
  const int64_t duration_ms = run ? run->duration_ms : 0;

  ProcessOptions process;
  process.args = args;
  process.timeout = std::chrono::milliseconds(timeout_for_segment_ms(duration_ms));
  process.stderr_tail_limit = 8192;

  const ProcessResult result = run_process(process);
  if (!result.started) {
    throw DependencyError("cannot start ffmpeg: " + result.error);
  }
  if (result.timed_out) {
    throw InternalError("segment timed out after " + std::to_string(process.timeout.count()) + "ms");
  }
  if (!result.ok()) {
    throw InputError("segment failed: " + trim(result.stderr_tail));
  }

  const std::vector<fs::path> files = list_clip_files(tmp_clips);
  if (files.empty()) {
    throw InternalError("segment produced no clips");
  }

  const fs::path final_dir = paths_.clips_dir(run_id);
  std::vector<ClipRow> clips;
  clips.reserve(files.size());

  int64_t start_ms = 0;
  for (const fs::path& file : files) {
    const int64_t index = clip_number(file);
    if (index < 0) {
      throw InternalError("unexpected clip filename: " + file.string());
    }
    ProbeInfo info = probe_media(file, std::chrono::milliseconds(kProbeTimeoutMs));
    if (info.duration_ms <= 0) {
      throw InternalError("clip has no duration: " + file.string());
    }
    int64_t frames = info.nb_frames;
    if (frames <= 0 || info.nb_frames_estimated) {
      const int64_t timeout = std::max<int64_t>(60000, info.duration_ms * 2);
      frames = count_video_frames(file, std::chrono::milliseconds(timeout));
    }

    ClipRow clip;
    clip.run_id = run_id;
    clip.idx = index;
    clip.clip_id = make_clip_id(run_id, clip.idx);
    clip.path = absolute_path(final_dir / file.filename());
    clip.start_ms = start_ms;
    clip.end_ms = start_ms + info.duration_ms;
    clip.frames = frames;
    start_ms = clip.end_ms;
    clips.push_back(std::move(clip));
  }

  remove_all_checked(final_dir);
  std::error_code ec;
  fs::rename(tmp_clips, final_dir, ec);
  if (ec) {
    throw InternalError("cannot publish clips: " + ec.message());
  }

  db_->replace_clips(run_id, clips);
  emit("segment: " + std::to_string(clips.size()) + " clip(s) [" + format_duration(start_ms) + "]");
}

void Pipeline::run_thumb(const std::string& run_id) {
  const fs::path dest = paths_.thumb_path(run_id);
  if (file_ok(dest)) {
    emit("thumb: exists", false);
    return;
  }

  const fs::path input = paths_.normalized_path(run_id);
  if (!file_ok(input)) {
    emit("warning: thumbnail skipped, normalized file missing");
    return;
  }

  const fs::path tmp_dir = paths_.tmp_run_dir(run_id);
  ensure_dir(tmp_dir);
  const fs::path tmp = tmp_dir / "thumb.jpg";

  const auto run = db_->get_run(run_id);
  const int64_t duration_ms = run ? run->duration_ms : 0;
  const auto args = thumb_command(input, tmp, duration_ms);

  ProcessOptions process;
  process.args = args;
  process.timeout = std::chrono::milliseconds(kThumbTimeoutMs);
  process.stderr_tail_limit = 4096;

  const ProcessResult result = run_process(process);
  if (!result.ok() || !file_ok(tmp)) {
    emit("warning: thumbnail failed: " +
         (result.timed_out ? std::string("timeout") : trim(result.stderr_tail)));
    return;
  }

  std::error_code ec;
  fs::remove(dest, ec);
  fs::rename(tmp, dest, ec);
  if (ec) {
    emit("warning: cannot publish thumbnail: " + ec.message());
    return;
  }
  emit("thumb: ok", false);
}

void Pipeline::run_hash(const std::string& run_id) {
  const fs::path normalized = paths_.normalized_path(run_id);
  auto run = db_->get_run(run_id);
  if (!run) {
    throw InternalError("run disappeared during hashing: " + run_id);
  }

  if (run->normalized_sha256.empty()) {
    if (!file_ok(normalized)) {
      throw InternalError("normalized file is missing before hashing");
    }
    const FileStat stat = stat_file(normalized);
    const std::string sha =
        sha256_file(normalized, deadline_after_ms(timeout_for_hash_ms(stat.size)));
    db_->set_normalized_sha(run_id, sha);
  }

  for (const ClipRow& clip : db_->get_clips(run_id)) {
    if (!clip.sha256.empty()) {
      continue;
    }
    if (!file_ok(clip.path)) {
      throw InternalError("clip is missing before hashing: " + clip.path);
    }
    const FileStat stat = stat_file(clip.path);
    const std::string sha = sha256_file(clip.path, deadline_after_ms(timeout_for_hash_ms(stat.size)));
    db_->set_clip_hash(clip.clip_id, sha);
  }
  emit("hash: ok");
}

void Pipeline::write_meta(const std::string& run_id, const ProbeInfo& source_info) {
  const auto run = db_->get_run(run_id);
  if (!run) {
    throw InternalError("run disappeared before meta: " + run_id);
  }
  const auto source = db_->get_source(run->source_id);
  const std::vector<ClipRow> clips = db_->get_clips(run_id);

  nlohmann::json meta;
  meta["schema"] = 1;
  meta["run_id"] = run_id;
  meta["source_id"] = run->source_id;
  meta["source"] = {
      {"source_id", run->source_id},
      {"path", source ? source->path : source_info.path},
      {"sha256", source ? source->sha256 : std::string()},
      {"size", source ? source->size : source_info.size},
      {"width", source_info.width},
      {"height", source_info.height},
      {"fps", source_info.avg.value()},
      {"fps_num", source_info.fps.num},
      {"fps_den", source_info.fps.den},
      {"avg_num", source_info.avg.num},
      {"avg_den", source_info.avg.den},
      {"duration_ms", source_info.duration_ms},
      {"nb_frames", source_info.nb_frames},
      {"nb_frames_estimated", source_info.nb_frames_estimated},
      {"vfr", source_info.vfr},
      {"vfr_ratio", source_info.vfr_ratio},
      {"vfr_num", source_info.vfr_num},
      {"vfr_den", source_info.vfr_den},
      {"vfr_checked", source_info.vfr_checked},
      {"hdr", source_info.hdr},
      {"rotation", source_info.rotation},
      {"codec", source_info.codec},
      {"pix_fmt", source_info.pix_fmt},
      {"audio_streams", source_info.audio_streams},
  };
  meta["profile"] = {
      {"name", profile_.name},
      {"fp", profile_.fp()},
      {"params", profile_.params()},
  };
  meta["toolchain"] = toolchain_.to_json();

  const fs::path normalized_path = paths_.normalized_path(run_id);
  meta["normalized"] = {
      {"path", absolute_path(normalized_path)},
      {"sha256", run->normalized_sha256},
      {"duration_ms", run->duration_ms},
      {"width", profile_.width},
      {"height", profile_.height},
      {"fps", static_cast<double>(profile_.fps_num) / static_cast<double>(profile_.fps_den)},
  };

  nlohmann::json clips_json = nlohmann::json::array();
  for (const ClipRow& clip : clips) {
    clips_json.push_back({
        {"clip_id", clip.clip_id},
        {"idx", clip.idx},
        {"path", clip.path},
        {"rel_path", relative_path_string(clip.path, paths_.root)},
        {"start_ms", clip.start_ms},
        {"end_ms", clip.end_ms},
        {"frames", clip.frames},
        {"sha256", clip.sha256},
    });
  }
  meta["clips"] = std::move(clips_json);

  const fs::path clips_pattern = paths_.clips_dir(run_id) / "%04d.mp4";
  meta["commands"] = {
      {"normalize", normalize_command(fs::path(source ? source->path : source_info.path),
                                      normalized_path)},
      {"segment", segment_command(normalized_path, clips_pattern)},
      {"thumb", thumb_command(normalized_path, paths_.thumb_path(run_id), run->duration_ms)},
  };
  meta["state"] = "done";
  meta["created_at"] = run->created_at;
  meta["updated_at"] = run->updated_at;

  write_file_atomic(paths_.meta_path(run_id), meta.dump(2) + "\n");
}

ProcessOutcome Pipeline::process_path(const fs::path& input, bool retry_mode,
                                      const std::string& expected_source_id) {
  const fs::path source = fs::path(absolute_path(input));
  if (!fs::is_regular_file(source)) {
    throw InputError("not a regular file: " + source.string());
  }
  const FileStat stat = stat_file(source);
  stage_ = "queued";
  console("== " + source.string());

  std::string source_sha;
  try {
    source_sha = sha256_file(source, deadline_after_ms(timeout_for_hash_ms(stat.size)));
  } catch (const AppError& e) {
    throw InputError("cannot hash source " + source.string() + ": " + e.what());
  }

  const std::string source_id = make_source_id(source_sha);
  if (retry_mode && !expected_source_id.empty() && source_id != expected_source_id) {
    throw InputError("source content changed since it was catalogued: " + source.string());
  }

  ProbeOutcome probe = probe_source(source, std::chrono::milliseconds(kProbeTimeoutMs), true);
  const ProbeInfo& info = probe.info;
  const std::string profile_fp = profile_.fp();
  std::string run_id = make_run_id(source_id, profile_fp);

  const auto emit_probe_warnings = [&]() {
    for (const std::string& warning : probe.warnings) {
      emit("warning: " + warning);
    }
    if (info.hdr) {
      emit("warning: HDR source (" + info.color_transfer + "); output is SDR, no tonemap");
    }
    if (info.vfr) {
      std::ostringstream message;
      message << "warning: source is VFR (ratio " << info.vfr_ratio
              << "); normalizing to CFR";
      emit(message.str());
    }
  };

  if (options_.dry_run) {
    emit_probe_warnings();
    const int64_t segment_ms = static_cast<int64_t>(profile_.segment_seconds) * 1000;
    const int64_t predicted =
        segment_ms > 0 ? (info.duration_ms + segment_ms - 1) / segment_ms : 0;
    std::ostringstream out;
    out << "[dry-run] " << source.string() << "\n"
        << "  source_id: " << source_id << "\n"
        << "  run_id: " << run_id << "\n"
        << "  profile: " << profile_.name << " fp=" << profile_fp << "\n"
        << "  probe: " << info.width << "x" << info.height << " fps=" << info.fps.num << "/"
        << info.fps.den << " duration=" << format_duration(info.duration_ms)
        << " frames=" << info.nb_frames << " vfr=" << (info.vfr ? "yes" : "no")
        << " ratio=" << info.vfr_ratio << " hdr=" << (info.hdr ? "yes" : "no")
        << " rotation=" << info.rotation << "\n"
        << "  would normalize: " << profile_.width << "x" << profile_.height << " @ "
        << profile_fps_string(profile_) << " " << profile_.codec << "\n"
        << "  would segment: " << profile_.segment_seconds << "s -> about " << predicted
        << " clip(s)\n"
        << "  output: " << paths_.run_dir(run_id).string() << "\n";
    std::cout << out.str();
    std::cout.flush();
    return {OutcomeKind::DryRun, run_id, "dry run", 0};
  }

  SourceRow source_row;
  source_row.source_id = source_id;
  source_row.path = source.string();
  source_row.size = stat.size;
  source_row.mtime_ns = stat.mtime_ns;
  source_row.sha256 = source_sha;
  source_row.width = info.width;
  source_row.height = info.height;
  source_row.fps = info.fps;
  source_row.avg = info.avg;
  source_row.duration_ms = info.duration_ms;
  source_row.nb_frames = info.nb_frames;
  source_row.vfr = info.vfr;
  source_row.vfr_ratio = info.vfr_ratio;
  source_row.hdr = info.hdr;
  source_row.rotation = info.rotation;
  source_row.codec = info.codec;
  source_row.created_at = now_iso8601();
  db_->upsert_source(source_row);

  const auto existing = db_->find_run(source_id, profile_fp);
  if (existing) {
    run_id = existing->run_id;
    if (existing->state == "done" && !options_.force_reencode) {
      bool products_complete = file_ok(paths_.normalized_path(run_id)) &&
                               file_ok(paths_.thumb_path(run_id)) &&
                               file_ok(paths_.meta_path(run_id));
      const auto products = db_->get_clips(run_id);
      if (products.empty()) {
        products_complete = false;
      }
      for (const ClipRow& clip : products) {
        if (!file_ok(clip.path)) {
          products_complete = false;
          break;
        }
      }
      if (products_complete && !options_.no_hash) {
        products_complete = !existing->normalized_sha256.empty();
        for (const ClipRow& clip : products) {
          if (clip.sha256.empty()) {
            products_complete = false;
            break;
          }
        }
      }
      if (products_complete) {
        console("skip: already done (" + run_id + ")");
        return {OutcomeKind::Skipped, run_id, "already done", 0};
      }
      console("resume: run is marked done but its products are incomplete");
    }

    const bool artifacts = file_ok(paths_.normalized_path(run_id)) ||
                           db_->count_clips(run_id) > 0 || file_ok(paths_.meta_path(run_id));
    if (!existing->toolchain_fp.empty() && existing->toolchain_fp != toolchain_.fingerprint &&
        artifacts && !options_.force_reencode) {
      throw UsageError("toolchain changed for run " + run_id + " (" + existing->toolchain_fp +
                       " -> " + toolchain_.fingerprint + "); rerun with --force-reencode");
    }

    if (options_.force_reencode) {
      cleanup_derived(run_id);
      db_->set_toolchain(run_id, toolchain_.fingerprint);
      console("force reencode: cleared derived artifacts for " + run_id);
    } else if (existing->toolchain_fp != toolchain_.fingerprint) {
      db_->set_toolchain(run_id, toolchain_.fingerprint);
    }
  } else {
    RunRow row;
    row.run_id = run_id;
    row.source_id = source_id;
    row.profile = profile_.name;
    row.profile_fp = profile_fp;
    row.toolchain_fp = toolchain_.fingerprint;
    row.state = "queued";
    db_->insert_run(row);
  }

  db_->bump_attempt(run_id);
  start_log(run_id);
  emit_probe_warnings();
  emit("source_id=" + source_id + " run_id=" + run_id + " profile=" + profile_.name + " fp=" +
       profile_fp);
  emit("probe: " + std::to_string(info.width) + "x" + std::to_string(info.height) + " fps=" +
       std::to_string(info.fps.num) + "/" + std::to_string(info.fps.den) + " duration=" +
       format_duration(info.duration_ms) + " frames=" + std::to_string(info.nb_frames) +
       " vfr=" + (info.vfr ? "yes" : "no") + " hdr=" + (info.hdr ? "yes" : "no") +
       " rotation=" + std::to_string(info.rotation));

  try {
    db_->set_state(run_id, "probing", std::nullopt);
    stage_ = "probing";

    bool normalized_ok = file_ok(paths_.normalized_path(run_id));
    if (normalized_ok) {
      const auto run = db_->get_run(run_id);
      if (!run || run->normalized_path.empty() || run->duration_ms <= 0) {
        try {
          const ProbeInfo normalized_info =
              probe_media(paths_.normalized_path(run_id), std::chrono::milliseconds(kProbeTimeoutMs));
          db_->set_normalized(run_id, absolute_path(paths_.normalized_path(run_id)),
                              run ? run->normalized_sha256 : std::string(),
                              normalized_info.duration_ms);
        } catch (const AppError& e) {
          emit("warning: normalized file is unreadable (" + std::string(e.what()) +
               "); re-encoding");
          cleanup_derived(run_id);
          db_->set_toolchain(run_id, toolchain_.fingerprint);
          normalized_ok = false;
        }
      }
    }

    if (!normalized_ok) {
      stage_ = "normalizing";
      db_->set_state(run_id, "normalizing", std::nullopt);
      run_normalize(run_id, source, info);
    }

    auto clips = db_->get_clips(run_id);
    bool clip_rows_present = !clips.empty();
    bool clip_files_present = clip_rows_present;
    for (const ClipRow& clip : clips) {
      if (!file_ok(clip.path)) {
        clip_files_present = false;
        break;
      }
    }
    bool hashes_complete = clip_rows_present;
    if (!options_.no_hash) {
      for (const ClipRow& clip : clips) {
        if (clip.sha256.empty()) {
          hashes_complete = false;
          break;
        }
      }
      const auto run = db_->get_run(run_id);
      if (!run || run->normalized_sha256.empty()) {
        hashes_complete = false;
      }
    }

    const ResumePlan plan = decide_resume(normalized_ok, clip_rows_present,
                                          clip_files_present, hashes_complete, options_.no_hash);
    if (!plan.clips_ok) {
      stage_ = "segmenting";
      db_->set_state(run_id, "segmenting", std::nullopt);
      run_segment(run_id);
      clips = db_->get_clips(run_id);
    }

    if (!options_.no_hash) {
      const auto run = db_->get_run(run_id);
      bool all_clips_hashed = run && !run->normalized_sha256.empty();
      for (const ClipRow& clip : clips) {
        if (clip.sha256.empty()) {
          all_clips_hashed = false;
          break;
        }
      }
      if (!all_clips_hashed) {
        stage_ = "hashing";
        db_->set_state(run_id, "hashing", std::nullopt);
        run_hash(run_id);
      }
    }

    stage_ = "thumb";
    run_thumb(run_id);

    stage_ = "meta";
    write_meta(run_id, info);

    remove_all_quiet(paths_.tmp_run_dir(run_id));
    db_->set_state(run_id, "done", std::nullopt);
    const auto done_run = db_->get_run(run_id);
    const int64_t clip_count = db_->count_clips(run_id);
    emit("done: " + std::to_string(clip_count) + " clip(s), " +
         format_duration(done_run ? done_run->duration_ms : 0));
    stop_log();
    return {OutcomeKind::Done, run_id, "done", 0};
  } catch (const AppError& e) {
    const std::string message = stage_ + ": " + e.what();
    try {
      db_->set_state(run_id, "failed", message);
    } catch (...) {
    }
    emit("FAILED " + message);
    stop_log();
    return {OutcomeKind::Failed, run_id, message, e.exit_code()};
  } catch (const std::exception& e) {
    const std::string message = stage_ + ": " + std::string(e.what());
    try {
      db_->set_state(run_id, "failed", message);
    } catch (...) {
    }
    emit("FAILED " + message);
    stop_log();
    return {OutcomeKind::Failed, run_id, message, 5};
  }
}

}  // namespace cb
