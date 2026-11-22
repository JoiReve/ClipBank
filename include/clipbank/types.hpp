#pragma once

#include <cstdint>
#include <string>

namespace cb {

struct Rational {
    int64_t num = 0;
    int64_t den = 1;

    bool valid() const noexcept { return den > 0 && num > 0; }
    double value() const noexcept { return valid() ? static_cast<double>(num) / static_cast<double>(den) : 0.0; }
    bool operator==(const Rational& other) const noexcept {
        return num == other.num && den == other.den;
    }
};

// Parses ffprobe rationals such as "30000/1001", "30" and "24/1".
// Invalid, missing or "0/0" input yields {0,0} (invalid).
Rational parse_rational(const std::string& text);

// Everything clipbank learns from ffprobe/vfrdet about one input video.
struct ProbeInfo {
    std::string path;

    int width = 0;
    int height = 0;

    Rational fps; // r_frame_rate
    Rational avg; // avg_frame_rate

    int64_t duration_ms = 0;
    int64_t nb_frames = 0;
    bool nb_frames_estimated = false;

    bool vfr = false;
    double vfr_ratio = 0.0;
    int64_t vfr_num = 0;
    int64_t vfr_den = 0;
    bool vfr_checked = false;

    bool hdr = false;
    int rotation = 0;

    std::string codec;
    std::string pix_fmt;
    std::string color_transfer;
    std::string format_name;

    int audio_streams = 0;
    bool has_audio() const noexcept { return audio_streams > 0; }

    int64_t size = 0;
};

} // namespace cb
