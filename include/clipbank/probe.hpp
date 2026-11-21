#pragma once

#include "clipbank/types.hpp"
#include "clipbank/util.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace cb {

std::string ffmpeg_binary();
std::string ffprobe_binary();

struct ProbeOutcome {
  ProbeInfo info;
  std::vector<std::string> warnings;
};

// ffprobe metadata + vfrdet (unless detect_vfr is false).
ProbeOutcome probe_source(const fs::path& path,
                          std::chrono::milliseconds timeout = std::chrono::milliseconds(60000),
                          bool detect_vfr = true);

// ffprobe metadata only; used by verify and by resume checks.
ProbeInfo probe_media(const fs::path& path,
                      std::chrono::milliseconds timeout = std::chrono::milliseconds(60000));

// Exposed for unit tests.
ProbeInfo parse_ffprobe_json(const nlohmann::json& json, const std::string& path, int64_t file_size);
bool parse_vfrdet_line(const std::string& line, double& ratio, int64_t& num, int64_t& den);

// Threshold used everywhere: ratio > 1% means VFR.
constexpr double kVfrThreshold = 0.01;

std::optional<bool> first_frame_is_keyframe(
    const fs::path& path,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(60000));

int64_t count_video_frames(const fs::path& path,
                           std::chrono::milliseconds timeout = std::chrono::milliseconds(600000));

struct Toolchain {
  std::string ffmpeg_version;
  std::string ffprobe_version;
  std::string encoder = "libx264";
  std::string fingerprint;

  static Toolchain detect();
  nlohmann::json to_json() const;
};

void ensure_dependencies();
void ensure_ffprobe_available();

}  // namespace cb
