#include "clipbank/probe.hpp"

#include "clipbank/errors.hpp"
#include "clipbank/sha256.hpp"
#include "clipbank/subprocess.hpp"
#include "clipbank/util.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <set>
#include <string_view>

#include <sys/stat.h>

namespace cb {
namespace {

std::optional<std::string> env_string(const char* name) {
  if (const char* value = std::getenv(name); value != nullptr && value[0] != '\0') {
    return std::string(value);
  }
  return std::nullopt;
}

std::optional<int64_t> json_int(const nlohmann::json& value) {
  if (value.is_number_integer()) {
    return value.get<int64_t>();
  }
  if (value.is_number_unsigned()) {
    return static_cast<int64_t>(value.get<uint64_t>());
  }
  if (value.is_number_float()) {
    return static_cast<int64_t>(std::llround(value.get<double>()));
  }
  if (value.is_string()) {
    const std::string text = value.get<std::string>();
    if (text.empty() || text == "N/A") {
      return std::nullopt;
    }
    int64_t parsed = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (ec == std::errc() && ptr == text.data() + text.size()) {
      return parsed;
    }
  }
  return std::nullopt;
}

std::optional<double> json_double(const nlohmann::json& value) {
  if (value.is_number()) {
    return value.get<double>();
  }
  if (value.is_string()) {
    const std::string text = value.get<std::string>();
    if (text.empty() || text == "N/A") {
      return std::nullopt;
    }
    try {
      std::size_t consumed = 0;
      const double parsed = std::stod(text, &consumed);
      if (consumed == text.size()) {
        return parsed;
      }
    } catch (...) {
      return std::nullopt;
    }
  }
  return std::nullopt;
}

std::optional<std::string> json_string(const nlohmann::json& value) {
  if (value.is_string()) {
    const std::string text = value.get<std::string>();
    if (!text.empty() && text != "N/A") {
      return text;
    }
  }
  return std::nullopt;
}

std::optional<int64_t> object_int(const nlohmann::json& object, const char* key) {
  if (!object.is_object()) {
    return std::nullopt;
  }
  const auto it = object.find(key);
  return it == object.end() ? std::nullopt : json_int(*it);
}

std::optional<double> object_double(const nlohmann::json& object, const char* key) {
  if (!object.is_object()) {
    return std::nullopt;
  }
  const auto it = object.find(key);
  return it == object.end() ? std::nullopt : json_double(*it);
}

std::optional<std::string> object_string(const nlohmann::json& object, const char* key) {
  if (!object.is_object()) {
    return std::nullopt;
  }
  const auto it = object.find(key);
  return it == object.end() ? std::nullopt : json_string(*it);
}

std::string first_line(const std::string& text) {
  const std::size_t pos = text.find('\n');
  return pos == std::string::npos ? text : text.substr(0, pos);
}

std::string parse_version_token(const std::string& line, const std::string& prefix) {
  const std::size_t pos = line.find(prefix);
  if (pos == std::string::npos) {
    return "";
  }
  std::size_t begin = pos + prefix.size();
  std::size_t end = begin;
  while (end < line.size() && line[end] != ' ' && line[end] != '\t' && line[end] != '\r') {
    ++end;
  }
  return line.substr(begin, end - begin);
}

nlohmann::json run_ffprobe_json(const fs::path& path, std::chrono::milliseconds timeout) {
  ProcessOptions options;
  options.args = {ffprobe_binary(), "-v", "error", "-print_format", "json",
                  "-show_format", "-show_streams", path.string()};
  options.timeout = timeout;
  options.capture_stdout = true;
  options.stderr_tail_limit = 4096;

  const ProcessResult result = run_process(options);
  if (!result.started) {
    throw DependencyError("cannot start ffprobe: " + result.error);
  }
  if (result.timed_out) {
    throw InputError("ffprobe timed out: " + path.string());
  }
  if (!result.ok()) {
    throw InputError("ffprobe failed for " + path.string() + ": " + trim(result.stderr_tail));
  }

  nlohmann::json json;
  try {
    json = nlohmann::json::parse(result.stdout_text);
  } catch (const std::exception& e) {
    throw InputError("cannot parse ffprobe output for " + path.string() + ": " + e.what());
  }
  return json;
}

}  // namespace

std::string ffmpeg_binary() {
  return env_string("CLIPBANK_FFMPEG").value_or("ffmpeg");
}

std::string ffprobe_binary() {
  return env_string("CLIPBANK_FFPROBE").value_or("ffprobe");
}

Rational parse_rational(const std::string& text) {
  if (text.empty() || text == "N/A" || text == "0/0") {
    return {};
  }
  const std::size_t slash = text.find('/');
  try {
    if (slash == std::string::npos) {
      const int64_t num = std::stoll(text);
      return {num, 1};
    }
    const int64_t num = std::stoll(text.substr(0, slash));
    const int64_t den = std::stoll(text.substr(slash + 1));
    if (num < 0 || den <= 0) {
      return {};
    }
    return {num, den};
  } catch (...) {
    return {};
  }
}

ProbeInfo parse_ffprobe_json(const nlohmann::json& json, const std::string& path, int64_t file_size) {
  if (!json.contains("streams") || !json["streams"].is_array()) {
    throw InputError("ffprobe output has no streams array: " + path);
  }

  const nlohmann::json* video = nullptr;
  int audio_streams = 0;
  for (const auto& stream : json["streams"]) {
    const auto type = object_string(stream, "codec_type");
    if (type && *type == "audio") {
      ++audio_streams;
    }
    bool attached_pic = false;
    if (const auto& disposition = stream.find("disposition");
        disposition != stream.end() && disposition->is_object()) {
      attached_pic = object_int(*disposition, "attached_pic").value_or(0) != 0;
    }
    if (type && *type == "video" && !attached_pic && video == nullptr) {
      video = &stream;
    }
  }
  if (video == nullptr) {
    throw InputError("no video stream: " + path);
  }

  ProbeInfo info;
  info.path = path;
  info.width = static_cast<int>(object_int(*video, "width").value_or(0));
  info.height = static_cast<int>(object_int(*video, "height").value_or(0));
  if (info.width <= 0 || info.height <= 0) {
    throw InputError("video stream has no usable dimensions: " + path);
  }

  info.codec = object_string(*video, "codec_name").value_or("");
  info.pix_fmt = object_string(*video, "pix_fmt").value_or("");
  info.color_transfer = object_string(*video, "color_transfer").value_or("");
  info.hdr = info.color_transfer == "smpte2084" || info.color_transfer == "arib-std-b67";
  info.audio_streams = audio_streams;

  info.fps = parse_rational(object_string(*video, "r_frame_rate").value_or(""));
  info.avg = parse_rational(object_string(*video, "avg_frame_rate").value_or(""));
  if (!info.fps.valid()) {
    info.fps = info.avg;
  }
  if (!info.avg.valid()) {
    info.avg = info.fps;
  }

  int64_t rotation = 0;
  if (const auto& side_data = video->find("side_data_list");
      side_data != video->end() && side_data->is_array()) {
    for (const auto& item : *side_data) {
      if (const auto value = object_int(item, "rotation"); value) {
        rotation = *value;
        break;
      }
    }
  }
  if (rotation == 0) {
    if (const auto& tags = video->find("tags"); tags != video->end() && tags->is_object()) {
      if (const auto value = object_int(*tags, "rotate"); value) {
        rotation = *value;
      }
    }
  }
  rotation %= 360;
  if (rotation < 0) {
    rotation += 360;
  }
  info.rotation = static_cast<int>(rotation);

  const nlohmann::json* format = json.contains("format") && json["format"].is_object()
                                     ? &json["format"]
                                     : nullptr;

  std::optional<double> duration_seconds = object_double(*video, "duration");
  if (!duration_seconds && format != nullptr) {
    duration_seconds = object_double(*format, "duration");
  }
  if (duration_seconds && *duration_seconds > 0.0) {
    info.duration_ms = static_cast<int64_t>(std::llround(*duration_seconds * 1000.0));
  }

  const auto nb_frames = object_int(*video, "nb_frames");
  if (nb_frames && *nb_frames > 0) {
    info.nb_frames = *nb_frames;
  } else if (info.duration_ms > 0) {
    const Rational rate = info.avg.valid() ? info.avg : info.fps;
    if (rate.valid()) {
      info.nb_frames = static_cast<int64_t>(
          std::llround(static_cast<double>(info.duration_ms) / 1000.0 * rate.value()));
      info.nb_frames_estimated = true;
    }
  }

  if (info.duration_ms <= 0 && info.nb_frames > 0 && info.avg.valid()) {
    info.duration_ms = static_cast<int64_t>(
        std::llround(static_cast<double>(info.nb_frames) / info.avg.value() * 1000.0));
  }
  if (info.duration_ms <= 0) {
    throw InputError("cannot determine video duration: " + path);
  }

  info.format_name = format != nullptr ? object_string(*format, "format_name").value_or("") : "";
  info.size = object_int(*format, "size").value_or(file_size);
  return info;
}

bool parse_vfrdet_line(const std::string& line, double& ratio, int64_t& num, int64_t& den) {
  const std::size_t marker = line.find("VFR:");
  if (marker == std::string::npos) {
    return false;
  }
  const std::size_t value_begin = marker + 4;
  std::size_t value_end = value_begin;
  while (value_end < line.size() &&
         ((line[value_end] >= '0' && line[value_end] <= '9') || line[value_end] == '.')) {
    ++value_end;
  }
  if (value_end == value_begin) {
    return false;
  }

  const std::size_t open = line.find('(', value_end);
  const std::size_t slash = open == std::string::npos ? std::string::npos : line.find('/', open);
  const std::size_t close = slash == std::string::npos ? std::string::npos : line.find(')', slash);
  if (open == std::string::npos || slash == std::string::npos || close == std::string::npos) {
    return false;
  }

  try {
    ratio = std::stod(line.substr(value_begin, value_end - value_begin));
    num = std::stoll(line.substr(open + 1, slash - open - 1));
    den = std::stoll(line.substr(slash + 1, close - slash - 1));
    return true;
  } catch (...) {
    return false;
  }
}

ProbeOutcome probe_source(const fs::path& path, std::chrono::milliseconds timeout, bool detect_vfr) {
  const FileStat stat = stat_file(path);
  const nlohmann::json json = run_ffprobe_json(path, timeout);

  ProbeOutcome outcome;
  outcome.info = parse_ffprobe_json(json, path.string(), stat.size);

  if (!detect_vfr) {
    return outcome;
  }

  ProcessOptions options;
  options.args = {ffmpeg_binary(), "-hide_banner", "-nostdin", "-nostats", "-i", path.string(),
                  "-map", "0:V:0", "-vf", "vfrdet", "-f", "null", "-"};
  options.timeout = timeout;
  options.stderr_tail_limit = 16384;

  bool found = false;
  double ratio = 0.0;
  int64_t num = 0;
  int64_t den = 0;
  options.on_stderr_line = [&](const std::string& line) {
    if (!found && parse_vfrdet_line(line, ratio, num, den)) {
      found = true;
    }
  };

  const ProcessResult result = run_process(options);
  if (found) {
    outcome.info.vfr_checked = true;
    outcome.info.vfr_ratio = ratio;
    outcome.info.vfr_num = num;
    outcome.info.vfr_den = den;
    outcome.info.vfr = ratio > kVfrThreshold;
  } else if (result.timed_out) {
    outcome.warnings.push_back("vfrdet timed out; vfr left unchecked");
  } else if (!result.ok()) {
    outcome.warnings.push_back("vfrdet failed; vfr left unchecked: " + trim(result.stderr_tail));
  } else {
    outcome.warnings.push_back("vfrdet produced no ratio; vfr left unchecked");
  }

  return outcome;
}

ProbeInfo probe_media(const fs::path& path, std::chrono::milliseconds timeout) {
  const FileStat stat = stat_file(path);
  const nlohmann::json json = run_ffprobe_json(path, timeout);
  return parse_ffprobe_json(json, path.string(), stat.size);
}

std::optional<bool> first_frame_is_keyframe(const fs::path& path, std::chrono::milliseconds timeout) {
  ProcessOptions options;
  options.args = {ffprobe_binary(), "-v", "error", "-select_streams", "v:0",
                  "-read_intervals", "%+#1", "-show_entries", "frame=key_frame",
                  "-of", "default=nw=1:nk=1", path.string()};
  options.timeout = timeout;
  options.capture_stdout = true;

  const ProcessResult result = run_process(options);
  if (!result.ok()) {
    return std::nullopt;
  }
  const std::string value = trim(first_line(result.stdout_text));
  if (value == "1") {
    return true;
  }
  if (value == "0") {
    return false;
  }
  return std::nullopt;
}

int64_t count_video_frames(const fs::path& path, std::chrono::milliseconds timeout) {
  ProcessOptions options;
  options.args = {ffprobe_binary(), "-v", "error", "-select_streams", "v:0",
                  "-count_frames", "-show_entries", "stream=nb_read_frames",
                  "-of", "default=nw=1:nk=1", path.string()};
  options.timeout = timeout;
  options.capture_stdout = true;

  const ProcessResult result = run_process(options);
  if (!result.ok()) {
    throw InputError("cannot count frames in " + path.string() + ": " + trim(result.stderr_tail));
  }
  const std::string value = trim(first_line(result.stdout_text));
  try {
    const int64_t frames = std::stoll(value);
    if (frames >= 0) {
      return frames;
    }
  } catch (...) {
  }
  throw InputError("unexpected frame count for " + path.string() + ": " + value);
}

Toolchain Toolchain::detect() {
  Toolchain toolchain;

  ProcessOptions version_options;
  version_options.args = {ffmpeg_binary(), "-hide_banner", "-version"};
  version_options.timeout = std::chrono::milliseconds(20000);
  version_options.capture_stdout = true;
  version_options.stderr_tail_limit = 2048;
  const ProcessResult ffmpeg_version = run_process(version_options);
  if (!ffmpeg_version.started || !ffmpeg_version.ok()) {
    throw DependencyError("ffmpeg is not available: " +
                          (ffmpeg_version.error.empty() ? trim(ffmpeg_version.stderr_tail)
                                                        : ffmpeg_version.error));
  }
  toolchain.ffmpeg_version =
      parse_version_token(first_line(ffmpeg_version.stdout_text), "ffmpeg version ");
  if (toolchain.ffmpeg_version.empty()) {
    toolchain.ffmpeg_version = first_line(ffmpeg_version.stdout_text);
  }

  ProcessOptions probe_options;
  probe_options.args = {ffprobe_binary(), "-hide_banner", "-version"};
  probe_options.timeout = std::chrono::milliseconds(20000);
  probe_options.capture_stdout = true;
  probe_options.stderr_tail_limit = 2048;
  const ProcessResult ffprobe_version = run_process(probe_options);
  if (!ffprobe_version.started || !ffprobe_version.ok()) {
    throw DependencyError("ffprobe is not available: " +
                          (ffprobe_version.error.empty() ? trim(ffprobe_version.stderr_tail)
                                                        : ffprobe_version.error));
  }
  toolchain.ffprobe_version =
      parse_version_token(first_line(ffprobe_version.stdout_text), "ffprobe version ");
  if (toolchain.ffprobe_version.empty()) {
    toolchain.ffprobe_version = first_line(ffprobe_version.stdout_text);
  }

  ProcessOptions encoders_options;
  encoders_options.args = {ffmpeg_binary(), "-hide_banner", "-encoders"};
  encoders_options.timeout = std::chrono::milliseconds(20000);
  encoders_options.capture_stdout = true;
  encoders_options.stderr_tail_limit = 2048;
  const ProcessResult encoders = run_process(encoders_options);
  if (!encoders.ok() || encoders.stdout_text.find("libx264") == std::string::npos) {
    throw DependencyError("ffmpeg has no libx264 encoder");
  }

  toolchain.fingerprint =
      Sha256::hex_digest(toolchain.ffmpeg_version + "\n" + toolchain.encoder).substr(0, 8);
  return toolchain;
}

nlohmann::json Toolchain::to_json() const {
  return nlohmann::json{{"ffmpeg", ffmpeg_version},
                        {"encoder", encoder},
                        {"fp", fingerprint}};
}

void ensure_dependencies() {
  (void)Toolchain::detect();
}

void ensure_ffprobe_available() {
  ProcessOptions options;
  options.args = {ffprobe_binary(), "-hide_banner", "-version"};
  options.timeout = std::chrono::milliseconds(20000);
  options.capture_stdout = true;
  options.stderr_tail_limit = 2048;
  const ProcessResult result = run_process(options);
  if (!result.started || !result.ok()) {
    throw DependencyError("ffprobe is not available: " +
                          (result.error.empty() ? trim(result.stderr_tail) : result.error));
  }
}

}  // namespace cb
