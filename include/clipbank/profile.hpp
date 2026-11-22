#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

namespace cb {

struct Profile {
    std::string name = "detect";
    int width = 1920;
    int height = 1080;
    int fps_num = 30;
    int fps_den = 1;
    std::string codec = "h264";
    int crf = 20;
    std::string preset = "veryfast";
    std::string pix_fmt = "yuv420p";
    int segment_seconds = 60;
    bool audio = false;

    static Profile detect();

    // Fields shown in manifest.profile_params (identity without the display name).
    nlohmann::json params() const;

    // Canonical profile JSON; nlohmann sorts object keys, and dump() emits no
    // insignificant whitespace, so this representation is stable.
    nlohmann::json to_json() const;
    std::string canonical() const;
    std::string fp() const;
};

std::string make_source_id(const std::string& sha256_hex);
std::string make_run_id(const std::string& source_id, const std::string& profile_fp);
std::string make_clip_id(const std::string& run_id, int64_t index);

} // namespace cb
