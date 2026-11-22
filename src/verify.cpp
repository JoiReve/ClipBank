#include "clipbank/verify.hpp"

#include "clipbank/errors.hpp"
#include "clipbank/pipeline.hpp"
#include "clipbank/probe.hpp"
#include "clipbank/profile.hpp"
#include "clipbank/util.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <set>

namespace cb {
namespace {

constexpr int64_t kProbeTimeoutMs = 60000;

void add_error(VerifyRunResult& result, const std::string& message) {
    result.ok = false;
    result.errors.push_back(message);
}

void add_warning(VerifyRunResult& result, const std::string& message) {
    result.warnings.push_back(message);
}

bool file_ok(const fs::path& path) {
    std::error_code ec;
    return fs::is_regular_file(path, ec) && fs::file_size(path, ec) > 0 && !ec;
}

std::string fps_text(const Rational& fps) {
    return std::to_string(fps.num) + "/" + std::to_string(fps.den);
}

bool fps_matches(const Rational& fps, int num, int den) {
    if (!fps.valid()) {
        return false;
    }
    const double expected = static_cast<double>(num) / static_cast<double>(den);
    return std::abs(fps.value() - expected) <= 0.001;
}

bool path_name_in(const std::set<std::string>& names, const fs::path& path) {
    return names.count(path.filename().string()) != 0;
}

void check_media_spec(VerifyRunResult& result, const std::string& label,
                      const ProbeInfo& info) {
    const Profile profile = Profile::detect();
    if (info.width != profile.width || info.height != profile.height) {
        add_error(result, label + ": dimensions " + std::to_string(info.width) + "x" +
                              std::to_string(info.height) + " (expected " +
                              std::to_string(profile.width) + "x" + std::to_string(profile.height) + ")");
    }
    if (info.width % 2 != 0 || info.height % 2 != 0) {
        add_error(result, label + ": odd dimensions");
    }
    if (info.codec != profile.codec) {
        add_error(result, label + ": codec " + info.codec + " (expected " + profile.codec + ")");
    }
    if (info.pix_fmt != profile.pix_fmt) {
        add_error(result, label + ": pixel format " + info.pix_fmt + " (expected " + profile.pix_fmt + ")");
    }
    if (!fps_matches(info.fps, profile.fps_num, profile.fps_den)) {
        add_error(result, label + ": frame rate " + fps_text(info.fps) + " (expected " +
                              std::to_string(profile.fps_num) + "/" +
                              std::to_string(profile.fps_den) + ")");
    }
    if (info.has_audio()) {
        add_error(result, label + ": audio stream present");
    }
}

} // namespace

int64_t normalized_duration_tolerance_ms(int64_t reference_ms) {
    return std::max<int64_t>(500, reference_ms / 100);
}

int64_t clip_duration_tolerance_ms() {
    return 100;
}

bool frames_close(int64_t actual, int64_t expected) {
    return std::llabs(actual - expected) <= 1;
}

VerifySummary verify_library(const LibraryPaths& paths, Db& db, const VerifyOptions& options) {
    VerifySummary summary;

    std::vector<RunRow> runs;
    if (options.run_id.empty()) {
        runs = db.list_done_runs();
    } else {
        const auto run = db.get_run(options.run_id);
        if (!run) {
            throw UsageError("unknown run: " + options.run_id);
        }
        runs.push_back(*run);
    }

    const Profile profile = Profile::detect();

    for (const RunRow& run : runs) {
        VerifyRunResult result;
        result.run_id = run.run_id;
        result.clip_count = db.count_clips(run.run_id);

        if (run.state != "done") {
            add_error(result, "run state is " + run.state + " (expected done)");
        }
        if (run.profile != "detect") {
            add_error(result, "unsupported profile: " + run.profile);
        }
        if (run.profile_fp != profile.fp()) {
            add_error(result, "profile fingerprint " + run.profile_fp + " differs from current " +
                                  profile.fp());
        }

        const auto source = db.get_source(run.source_id);
        if (!source) {
            add_error(result, "source row missing: " + run.source_id);
        } else {
            result.source_path = source->path;
            if (source->vfr) {
                add_warning(result, "source is VFR (ratio " + std::to_string(source->vfr_ratio) + ")");
            }
            if (source->hdr) {
                add_warning(result, "source is HDR; output is SDR");
            }
        }

        const fs::path normalized_path =
            run.normalized_path.empty() ? paths.normalized_path(run.run_id)
                                        : fs::path(run.normalized_path);
        if (!file_ok(normalized_path)) {
            add_error(result, "normalized file missing: " + normalized_path.string());
            summary.runs.push_back(std::move(result));
            continue;
        }

        std::optional<ProbeInfo> normalized;
        try {
            normalized = probe_media(normalized_path, std::chrono::milliseconds(kProbeTimeoutMs));
        } catch (const AppError& e) {
            add_error(result, "cannot probe normalized file: " + std::string(e.what()));
            summary.runs.push_back(std::move(result));
            continue;
        }

        check_media_spec(result, "normalized", *normalized);

        if (run.duration_ms <= 0) {
            add_error(result, "run duration is missing");
        } else if (std::llabs(normalized->duration_ms - run.duration_ms) >
                   normalized_duration_tolerance_ms(run.duration_ms)) {
            add_error(result, "normalized duration " + std::to_string(normalized->duration_ms) +
                                  "ms != recorded " + std::to_string(run.duration_ms) + "ms");
        }
        if (source &&
            std::llabs(normalized->duration_ms - source->duration_ms) >
                normalized_duration_tolerance_ms(source->duration_ms)) {
            add_error(result, "normalized duration " + std::to_string(normalized->duration_ms) +
                                  "ms != source duration " + std::to_string(source->duration_ms) + "ms");
        }

        if (!options.no_hash) {
            if (run.normalized_sha256.empty()) {
                add_error(result, "normalized sha256 is missing");
            } else {
                try {
                    const FileStat stat = stat_file(normalized_path);
                    const std::string actual =
                        sha256_file(normalized_path, deadline_after_ms(timeout_for_hash_ms(stat.size)));
                    if (actual != run.normalized_sha256) {
                        add_error(result, "normalized sha256 mismatch");
                    }
                } catch (const AppError& e) {
                    add_error(result, "cannot hash normalized file: " + std::string(e.what()));
                }
            }
        }

        const std::vector<ClipRow> clips = db.get_clips(run.run_id);
        if (clips.empty()) {
            add_error(result, "no clips recorded");
        }

        const fs::path clips_dir = paths.clips_dir(run.run_id);
        if (!fs::is_directory(clips_dir)) {
            add_error(result, "clips directory missing: " + clips_dir.string());
        }

        std::set<std::string> expected_names;
        for (const ClipRow& clip : clips) {
            expected_names.insert(fs::path(clip.path).filename().string());
        }
        std::set<std::string> actual_names;
        std::error_code ec;
        if (fs::is_directory(clips_dir, ec)) {
            for (fs::directory_iterator it(clips_dir, ec), end; !ec && it != end; it.increment(ec)) {
                if (it->is_regular_file(ec) && it->path().extension() == ".mp4") {
                    actual_names.insert(it->path().filename().string());
                }
            }
            for (const std::string& name : actual_names) {
                if (expected_names.count(name) == 0) {
                    add_error(result, "unexpected clip file: " + name);
                }
            }
        }

        int64_t previous_end = 0;
        int64_t total_actual_frames = 0;
        bool all_actual_frames_known = true;
        for (std::size_t index = 0; index < clips.size(); ++index) {
            const ClipRow& clip = clips[index];
            const std::string label = "clip " + std::to_string(clip.idx);

            if (clip.clip_id != make_clip_id(run.run_id, clip.idx)) {
                add_error(result, label + ": clip_id mismatch (" + clip.clip_id + ")");
            }
            if (clip.end_ms < clip.start_ms) {
                add_error(result, label + ": negative duration range");
            }
            if (index == 0) {
                if (clip.start_ms != 0) {
                    add_error(result, label + ": first start_ms is " + std::to_string(clip.start_ms));
                }
            } else if (std::llabs(clip.start_ms - previous_end) > 1) {
                add_error(result, label + ": start_ms " + std::to_string(clip.start_ms) +
                                      " does not follow previous end_ms " + std::to_string(previous_end));
            }
            previous_end = clip.end_ms;

            const fs::path clip_path(clip.path);
            if (!file_ok(clip_path)) {
                add_error(result, label + ": file missing: " + clip.path);
                all_actual_frames_known = false;
                continue;
            }
            if (!path_name_in(expected_names, clip_path)) {
                add_error(result, label + ": path not in clips directory");
            }

            std::optional<ProbeInfo> info;
            try {
                info = probe_media(clip_path, std::chrono::milliseconds(kProbeTimeoutMs));
            } catch (const AppError& e) {
                add_error(result, label + ": cannot probe: " + std::string(e.what()));
                all_actual_frames_known = false;
                continue;
            }

            check_media_spec(result, label, *info);

            const int64_t expected_duration = clip.end_ms - clip.start_ms;
            if (std::llabs(info->duration_ms - expected_duration) > clip_duration_tolerance_ms()) {
                add_error(result, label + ": duration " + std::to_string(info->duration_ms) +
                                      "ms != recorded " + std::to_string(expected_duration) + "ms");
            }

            int64_t actual_frames = info->nb_frames;
            if (actual_frames <= 0 || info->nb_frames_estimated) {
                try {
                    actual_frames = count_video_frames(clip_path);
                } catch (const AppError& e) {
                    add_error(result, label + ": cannot count frames: " + std::string(e.what()));
                    actual_frames = -1;
                }
            }
            if (actual_frames >= 0) {
                total_actual_frames += actual_frames;
                if (!frames_close(actual_frames, clip.frames)) {
                    add_error(result, label + ": frames " + std::to_string(actual_frames) +
                                          " != recorded " + std::to_string(clip.frames));
                }
            } else {
                all_actual_frames_known = false;
            }

            if (clip.frames <= 0) {
                add_error(result, label + ": recorded frames is not positive");
            }

            const auto keyframe = first_frame_is_keyframe(clip_path);
            if (!keyframe) {
                add_error(result, label + ": cannot read first-frame keyframe flag");
            } else if (!*keyframe) {
                add_error(result, label + ": first frame is not a keyframe");
            }

            if (!options.no_hash) {
                if (clip.sha256.empty()) {
                    add_error(result, label + ": sha256 is missing");
                } else {
                    try {
                        const FileStat stat = stat_file(clip_path);
                        const std::string actual =
                            sha256_file(clip_path, deadline_after_ms(timeout_for_hash_ms(stat.size)));
                        if (actual != clip.sha256) {
                            add_error(result, label + ": sha256 mismatch");
                        }
                    } catch (const AppError& e) {
                        add_error(result, label + ": cannot hash: " + std::string(e.what()));
                    }
                }
            }
        }

        if (!clips.empty()) {
            const int64_t last_end = clips.back().end_ms;
            if (std::llabs(last_end - normalized->duration_ms) >
                normalized_duration_tolerance_ms(normalized->duration_ms)) {
                add_error(result, "last clip end " + std::to_string(last_end) +
                                      "ms != normalized duration " + std::to_string(normalized->duration_ms) +
                                      "ms");
            }
            if (all_actual_frames_known && normalized->nb_frames > 0 &&
                !normalized->nb_frames_estimated &&
                std::llabs(total_actual_frames - normalized->nb_frames) >
                    static_cast<int64_t>(clips.size())) {
                add_error(result, "clip frames sum " + std::to_string(total_actual_frames) +
                                      " != normalized frames " + std::to_string(normalized->nb_frames));
            }
        }

        const fs::path meta_path = paths.meta_path(run.run_id);
        if (!file_ok(meta_path)) {
            add_error(result, "meta.json missing");
        } else {
            try {
                const nlohmann::json meta = nlohmann::json::parse(read_file(meta_path));
                if (meta.value("schema", 0) != 1) {
                    add_error(result, "meta.json schema mismatch");
                }
                if (meta.value("run_id", std::string()) != run.run_id) {
                    add_error(result, "meta.json run_id mismatch");
                }
            } catch (const std::exception& e) {
                add_error(result, "meta.json is invalid: " + std::string(e.what()));
            }
        }

        if (!file_ok(paths.thumb_path(run.run_id))) {
            add_warning(result, "thumbnail missing");
        }

        summary.runs.push_back(std::move(result));
    }

    summary.ok = std::all_of(summary.runs.begin(), summary.runs.end(),
                             [](const VerifyRunResult& result) { return result.ok; });
    return summary;
}

nlohmann::json verify_to_json(const VerifySummary& summary) {
    nlohmann::json runs = nlohmann::json::array();
    for (const VerifyRunResult& result : summary.runs) {
        runs.push_back({
            {"run_id", result.run_id},
            {"source_path", result.source_path},
            {"ok", result.ok},
            {"clips", result.clip_count},
            {"errors", result.errors},
            {"warnings", result.warnings},
        });
    }
    return nlohmann::json{{"ok", summary.ok}, {"runs", std::move(runs)}};
}

} // namespace cb
