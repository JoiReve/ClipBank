#include "clipbank/db.hpp"
#include "clipbank/errors.hpp"
#include "clipbank/library.hpp"
#include "clipbank/manifest.hpp"
#include "clipbank/pipeline.hpp"
#include "clipbank/probe.hpp"
#include "clipbank/profile.hpp"
#include "clipbank/sha256.hpp"
#include "clipbank/subprocess.hpp"
#include "clipbank/util.hpp"
#include "clipbank/verify.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <regex>
#include <string>
#include <vector>

#include <unistd.h>

using namespace cb;

namespace {

struct TempDir {
    fs::path path;

    TempDir() {
        static int counter = 0;
        path = fs::temp_directory_path() /
               ("clipbank-unit-" + std::to_string(::getpid()) + "-" + std::to_string(counter++));
        fs::remove_all(path);
        fs::create_directories(path);
    }

    ~TempDir() { fs::remove_all(path); }
};

nlohmann::json video_stream_json() {
    return {
        {"streams",
         nlohmann::json::array(
             {{{"codec_type", "video"},
               {"width", 1280},
               {"height", 720},
               {"codec_name", "h264"},
               {"pix_fmt", "yuv420p"},
               {"r_frame_rate", "24/1"},
               {"avg_frame_rate", "8928/373"},
               {"duration", "15.541667"},
               {"nb_frames", "372"},
               {"color_transfer", "bt709"},
               {"side_data_list", nlohmann::json::array({{{"rotation", -90}}})}}})},
        {"format",
         {{"duration", "15.541667"}, {"format_name", "mov,mp4,m4a,3gp,3g2,mj2"}, {"size", "3977281"}}},
    };
}

} // namespace

TEST_CASE("sha256 known vectors") {
    CHECK(Sha256::hex_digest("") ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(Sha256::hex_digest("abc") ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(Sha256::hex_digest("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST_CASE("sha256 streaming matches one-shot") {
    const std::string data = "clipbank streaming hash test 0123456789";
    Sha256 sha;
    sha.update(data.substr(0, 7));
    sha.update(data.substr(7, 11));
    sha.update(data.substr(18));
    const auto digest = sha.final();
    CHECK(to_hex(digest.data(), digest.size()) == Sha256::hex_digest(data));
}

TEST_CASE("rational parsing") {
    CHECK(parse_rational("30000/1001").num == 30000);
    CHECK(parse_rational("30000/1001").den == 1001);
    CHECK(parse_rational("30").num == 30);
    CHECK(parse_rational("30").den == 1);
    CHECK_FALSE(parse_rational("0/0").valid());
    CHECK_FALSE(parse_rational("").valid());
    CHECK_FALSE(parse_rational("N/A").valid());
    CHECK_FALSE(parse_rational("abc").valid());
    CHECK(parse_rational("24/1").value() == doctest::Approx(24.0));
}

TEST_CASE("vfrdet line parser") {
    double ratio = 0.0;
    int64_t num = 0;
    int64_t den = 0;
    CHECK(parse_vfrdet_line(
        "[vfrdet @ 0x55d0] VFR:0.005391 (2/369) min:41.667 max:41.667 avg:41.670", ratio, num, den));
    CHECK(ratio == doctest::Approx(0.005391));
    CHECK(num == 2);
    CHECK(den == 369);
    CHECK_FALSE(ratio > kVfrThreshold);

    CHECK(parse_vfrdet_line("VFR:0.050000 (5/100) min:20 max:40", ratio, num, den));
    CHECK(ratio == doctest::Approx(0.05));
    CHECK(ratio > kVfrThreshold);

    CHECK_FALSE(parse_vfrdet_line("no ratio here", ratio, num, den));
}

TEST_CASE("ffprobe json parsing") {
    const auto info = parse_ffprobe_json(video_stream_json(), "/tmp/a.mp4", 123);
    CHECK(info.width == 1280);
    CHECK(info.height == 720);
    CHECK(info.codec == "h264");
    CHECK(info.pix_fmt == "yuv420p");
    CHECK(info.fps.num == 24);
    CHECK(info.fps.den == 1);
    CHECK(info.avg.num == 8928);
    CHECK(info.avg.den == 373);
    CHECK(info.duration_ms == 15542);
    CHECK(info.nb_frames == 372);
    CHECK_FALSE(info.nb_frames_estimated);
    CHECK(info.rotation == 270);
    CHECK_FALSE(info.hdr);
    CHECK_FALSE(info.has_audio());
    CHECK(info.size == 3977281);
}

TEST_CASE("ffprobe missing nb_frames falls back to duration x fps") {
    auto json = video_stream_json();
    json["streams"][0].erase("nb_frames");
    json["streams"][0]["duration"] = "10.0";
    json["format"]["duration"] = "10.0";
    json["streams"][0]["avg_frame_rate"] = "25/1";
    const auto info = parse_ffprobe_json(json, "/tmp/b.mp4", 1);
    CHECK(info.nb_frames == 250);
    CHECK(info.nb_frames_estimated);
    CHECK(info.duration_ms == 10000);
}

TEST_CASE("ffprobe HDR and rotation from tags") {
    auto json = video_stream_json();
    json["streams"][0]["color_transfer"] = "smpte2084";
    json["streams"][0].erase("side_data_list");
    json["streams"][0]["tags"] = {{"rotate", "90"}};
    const auto info = parse_ffprobe_json(json, "/tmp/hdr.mp4", 1);
    CHECK(info.hdr);
    CHECK(info.rotation == 90);
}

TEST_CASE("ffprobe skips attached cover-art streams") {
    nlohmann::json json = video_stream_json();
    nlohmann::json cover = {
        {"codec_type", "video"},
        {"codec_name", "mjpeg"},
        {"width", 64},
        {"height", 64},
        {"disposition", {{"attached_pic", 1}}},
    };
    json["streams"].insert(json["streams"].begin(), cover);
    const auto info = parse_ffprobe_json(json, "/tmp/cover.m4a", 1);
    CHECK(info.width == 1280);
    CHECK(info.height == 720);
    CHECK(info.codec == "h264");
}

TEST_CASE("ffprobe audio-only input is rejected") {
    nlohmann::json json = {
        {"streams",
         nlohmann::json::array(
             {{{"codec_type", "audio"}, {"codec_name", "aac"}}})},
        {"format", {{"duration", "10.0"}}},
    };
    CHECK_THROWS_AS(parse_ffprobe_json(json, "/tmp/audio.m4a", 1), InputError);
}

TEST_CASE("profile fingerprint is stable and params are visible") {
    const Profile a = Profile::detect();
    const Profile b = Profile::detect();
    CHECK(a.fp().size() == 8);
    CHECK(a.fp() == b.fp());
    CHECK(a.canonical() == b.canonical());
    CHECK(a.params()["width"] == 1920);
    CHECK(a.params()["fps_num"] == 30);
    CHECK(a.params()["segment_seconds"] == 60);

    Profile c = a;
    c.crf = 21;
    CHECK(c.fp() != a.fp());
}

TEST_CASE("id rules") {
    const std::string sha = Sha256::hex_digest("source");
    const std::string source_id = make_source_id(sha);
    CHECK(source_id.size() == 12);
    CHECK(source_id == sha.substr(0, 12));

    const std::string run_id = make_run_id(source_id, "a1b2c3d4");
    CHECK(run_id.size() == 12);
    CHECK(run_id == make_run_id(source_id, "a1b2c3d4"));

    CHECK(make_clip_id("a1b2c3d4e5f6", 0) == "a1b2c3d4e5f6-000");
    CHECK(make_clip_id("a1b2c3d4e5f6", 12) == "a1b2c3d4e5f6-012");
    CHECK(make_clip_id("a1b2c3d4e5f6", 1234) == "a1b2c3d4e5f6-1234");
}

TEST_CASE("ffmpeg progress parser") {
    int64_t us = 0;
    bool ended = false;

    CHECK(parse_ffmpeg_progress_line("out_time_us=1500000", us, ended));
    CHECK(us == 1500000);
    CHECK_FALSE(ended);

    CHECK(parse_ffmpeg_progress_line("out_time=00:00:01.500000", us, ended));
    CHECK(us == 1500000);

    CHECK(parse_ffmpeg_progress_line("out_time=01:02:03.250000", us, ended));
    CHECK(us == (1 * 3600LL + 2 * 60LL + 3) * 1000000LL + 250000);

    // ffmpeg 8's out_time_ms is microseconds; it must not be treated as ms.
    CHECK_FALSE(parse_ffmpeg_progress_line("out_time_ms=1500000", us, ended));

    CHECK(parse_ffmpeg_progress_line("progress=end", us, ended));
    CHECK(ended);
    CHECK(parse_ffmpeg_progress_line("progress=continue", us, ended));
    CHECK_FALSE(ended);
}

TEST_CASE("clip range accumulation uses real durations") {
    std::vector<int64_t> starts;
    std::vector<int64_t> ends;
    accumulate_clip_ranges({60000, 60000, 60000, 10000}, starts, ends);
    CHECK(starts == std::vector<int64_t>{0, 60000, 120000, 180000});
    CHECK(ends == std::vector<int64_t>{60000, 120000, 180000, 190000});

    accumulate_clip_ranges({}, starts, ends);
    CHECK(starts.empty());
    CHECK(ends.empty());
}

TEST_CASE("resume decisions") {
    CHECK(decide_resume(false, false, false, false, false).normalized_ok == false);
    CHECK(decide_resume(false, false, false, false, false).clips_ok == false);

    const ResumePlan fresh = decide_resume(false, false, false, false, false);
    CHECK_FALSE(fresh.clips_ok);
    CHECK_FALSE(fresh.hashes_ok);

    const ResumePlan middle = decide_resume(true, true, true, false, false);
    CHECK(middle.normalized_ok);
    CHECK(middle.clips_ok);
    CHECK_FALSE(middle.hashes_ok);

    const ResumePlan done = decide_resume(true, true, true, true, false);
    CHECK(done.hashes_ok);

    const ResumePlan no_hash = decide_resume(true, true, true, false, true);
    CHECK(no_hash.hashes_ok);
}

TEST_CASE("timeouts follow the plan") {
    CHECK(timeout_for_normalize_ms(1000) == 600000);
    CHECK(timeout_for_normalize_ms(200000) == 1200000);
    CHECK(timeout_for_segment_ms(1000) == 300000);
    CHECK(timeout_for_segment_ms(200000) == 400000);
    CHECK(timeout_for_hash_ms(0) == 300000);
    CHECK(timeout_for_hash_ms(100LL * 1024 * 1024) == 300000);
    CHECK(timeout_for_hash_ms(25LL * 1024 * 1024 * 1024) == 572000);
}

TEST_CASE("verify tolerances") {
    CHECK(frames_close(1800, 1800));
    CHECK(frames_close(1801, 1800));
    CHECK_FALSE(frames_close(1802, 1800));
    CHECK(normalized_duration_tolerance_ms(10000) == 500);
    CHECK(normalized_duration_tolerance_ms(190000) == 1900);
    CHECK(clip_duration_tolerance_ms() == 100);
}

TEST_CASE("subprocess captures output and exit status") {
    ProcessOptions options;
    options.args = {"/bin/sh", "-c", "printf 'hello\\n'; printf 'oops\\n' >&2; exit 3"};
    options.timeout = std::chrono::milliseconds(5000);
    options.capture_stdout = true;
    const ProcessResult result = run_process(options);

    CHECK(result.started);
    CHECK_FALSE(result.timed_out);
    CHECK(result.exit_code == 3);
    CHECK(result.stdout_text == "hello\n");
    CHECK(result.stderr_tail.find("oops") != std::string::npos);
}

TEST_CASE("subprocess timeout still applies after child closes pipes") {
    ProcessOptions options;
    // The child closes both pipes immediately, then sleeps. The parent must
    // still enforce the timeout instead of blocking in waitpid().
    options.args = {"/bin/sh", "-c", "exec >/dev/null 2>&1; sleep 5"};
    options.timeout = std::chrono::milliseconds(300);
    const auto start = std::chrono::steady_clock::now();
    const ProcessResult result = run_process(options);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();

    CHECK(result.started);
    CHECK(result.timed_out);
    CHECK(result.signalled);
    CHECK(elapsed < 3000);
}

TEST_CASE("video extension filter") {
    CHECK(is_video_extension("a.mp4"));
    CHECK(is_video_extension("a.MKV"));
    CHECK_FALSE(is_video_extension("a.txt"));
    CHECK_FALSE(is_video_extension("mp4"));
}

TEST_CASE("iso8601 timestamps are rfc3339") {
    const std::string now = now_iso8601();
    const std::regex pattern(R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}[+-]\d{2}:\d{2}$)");
    CHECK(std::regex_match(now, pattern));
}

TEST_CASE("manifest schema v1 snapshot") {
    TempDir temp;
    const LibraryPaths paths = resolve_library(temp.path);

    const std::string source_sha = Sha256::hex_digest("source-bytes");
    const std::string source_id = make_source_id(source_sha);
    const Profile profile = Profile::detect();
    const std::string run_id = make_run_id(source_id, profile.fp());
    const fs::path normalized = paths.normalized_path(run_id);
    const fs::path clip_path = paths.clips_dir(run_id) / "0000.mp4";

    {
        Db db(paths.db_path);
        db.ensure_schema();

        SourceRow source;
        source.source_id = source_id;
        source.path = "/tmp/source.mp4";
        source.size = 3977281;
        source.mtime_ns = 1;
        source.sha256 = source_sha;
        source.width = 1280;
        source.height = 720;
        source.fps = {24, 1};
        source.avg = {8928, 373};
        source.duration_ms = 15542;
        source.nb_frames = 372;
        source.vfr = false;
        source.vfr_ratio = 0.005391;
        source.hdr = false;
        source.rotation = 0;
        source.codec = "h264";
        source.created_at = now_iso8601();
        db.upsert_source(source);

        RunRow run;
        run.run_id = run_id;
        run.source_id = source_id;
        run.profile = profile.name;
        run.profile_fp = profile.fp();
        run.toolchain_fp = "abcd1234";
        db.insert_run(run);
        db.set_normalized(run_id, normalized.string(), "normalized-sha", 15533);

        ClipRow clip;
        clip.clip_id = make_clip_id(run_id, 0);
        clip.run_id = run_id;
        clip.idx = 0;
        clip.path = clip_path.string();
        clip.start_ms = 0;
        clip.end_ms = 15533;
        clip.frames = 466;
        clip.sha256 = "clip-sha";
        db.replace_clips(run_id, {clip});
        db.set_state(run_id, "done", std::nullopt);
    }

    Db db(paths.db_path);
    Toolchain toolchain;
    toolchain.ffmpeg_version = "8.0.1";
    toolchain.ffprobe_version = "8.0.1";
    toolchain.encoder = "libx264";
    toolchain.fingerprint = "abcd1234";
    const nlohmann::json manifest = build_manifest(paths, db, toolchain);

    CHECK(manifest["schema"] == 1);
    CHECK(manifest["profile"] == "detect");
    CHECK(manifest["profile_fp"] == profile.fp());
    CHECK(manifest["profile_params"]["width"] == 1920);
    CHECK(manifest["toolchain"]["fp"] == "abcd1234");
    CHECK(manifest["library"] == paths.root.string());
    REQUIRE(manifest["sources"].size() == 1);
    CHECK(manifest["sources"][0]["source_id"] == source_id);
    CHECK(manifest["sources"][0]["fps"].get<double>() ==
          doctest::Approx(8928.0 / 373.0));
    REQUIRE(manifest["clips"].size() == 1);
    CHECK(manifest["clips"][0]["clip_id"] == run_id + "-000");
    CHECK(manifest["clips"][0]["run_id"] == run_id);
    CHECK(manifest["clips"][0]["rel_path"] == run_id + "/clips/0000.mp4");
    CHECK(manifest["clips"][0]["start_ms"] == 0);
    CHECK(manifest["clips"][0]["end_ms"] == 15533);
    CHECK(manifest["clips"][0]["frames"] == 466);
    CHECK(manifest["clips"][0]["fps"] == 30.0);
    CHECK(manifest["clips"][0]["width"] == 1920);
    CHECK(manifest["clips"][0]["height"] == 1080);

    const std::regex timestamp_pattern(
        R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}[+-]\d{2}:\d{2}$)");
    CHECK(std::regex_match(manifest["generated_at"].get<std::string>(), timestamp_pattern));
}
