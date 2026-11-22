#include "clipbank/db.hpp"
#include "clipbank/errors.hpp"
#include "clipbank/library.hpp"
#include "clipbank/manifest.hpp"
#include "clipbank/pipeline.hpp"
#include "clipbank/probe.hpp"
#include "clipbank/profile.hpp"
#include "clipbank/util.hpp"
#include "clipbank/verify.hpp"

#include <CLI/CLI.hpp>

#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace cb;

std::string default_library() {
    if (const char* value = std::getenv("CLIPBANK_LIBRARY"); value != nullptr && value[0] != '\0') {
        return value;
    }
    return "library";
}

bool path_within(const fs::path& path, const fs::path& root) {
    const std::string p = absolute_path(path);
    const std::string r = absolute_path(root);
    if (p == r) {
        return true;
    }
    if (p.size() <= r.size() || p.compare(0, r.size(), r) != 0) {
        return false;
    }
    return p[r.size()] == fs::path::preferred_separator;
}

struct ScanResult {
    std::vector<fs::path> files;
    std::vector<std::string> errors;
};

void scan_input(const fs::path& input, const fs::path& library_root, ScanResult& result) {
    std::error_code ec;
    if (fs::is_directory(input, ec)) {
        fs::recursive_directory_iterator it(input, fs::directory_options::skip_permission_denied, ec);
        fs::recursive_directory_iterator end;
        for (; !ec && it != end; it.increment(ec)) {
            const fs::path path = it->path();
            if (path_within(path, library_root)) {
                if (it->is_directory(ec)) {
                    it.disable_recursion_pending();
                }
                continue;
            }
            if (it->is_regular_file(ec) && is_video_extension(path)) {
                result.files.push_back(path);
            }
        }
        if (ec) {
            result.errors.push_back("cannot scan " + input.string() + ": " + ec.message());
        } else if (result.files.empty()) {
            result.errors.push_back("no video files found under " + input.string());
        }
    } else if (fs::is_regular_file(input, ec)) {
        if (path_within(input, library_root)) {
            result.errors.push_back("refusing to ingest a file from inside the library: " +
                                    input.string());
        } else {
            result.files.push_back(input);
        }
    } else {
        result.errors.push_back("not found: " + input.string());
    }
}

std::vector<fs::path> dedupe_paths(std::vector<fs::path> files) {
    std::set<std::string> seen;
    std::vector<fs::path> out;
    for (const fs::path& file : files) {
        const std::string key = absolute_path(file);
        if (seen.insert(key).second) {
            out.push_back(file);
        }
    }
    std::sort(out.begin(), out.end(),
              [](const fs::path& a, const fs::path& b) { return absolute_path(a) < absolute_path(b); });
    return out;
}

int batch_exit_code(int current, int candidate) {
    if (candidate == 0) {
        return current;
    }
    return std::max(current, candidate);
}

int run_init(const std::string& library) {
    const LibraryPaths paths = init_library(library);
    std::cout << "library:  " << paths.root.string() << "\n"
              << "database: " << paths.db_path.string() << "\n"
              << "schema:   1\n";
    return 0;
}

int run_add(const std::string& library, const std::vector<std::string>& inputs,
            const std::string& profile_name, const PipelineOptions& options) {
    if (profile_name != "detect") {
        throw UsageError("unknown profile: " + profile_name);
    }

    const LibraryPaths paths = resolve_library(library);
    ScanResult scan;
    for (const std::string& input : inputs) {
        scan_input(input, paths.root, scan);
    }
    for (const std::string& error : scan.errors) {
        std::cerr << "clipbank: " << error << "\n";
    }
    const std::vector<fs::path> files = dedupe_paths(scan.files);
    if (files.empty()) {
        if (!scan.errors.empty()) {
            return 4;
        }
        throw UsageError("no video files to add");
    }

    const Profile profile = Profile::detect();
    const Toolchain toolchain = Toolchain::detect();

    LibraryPaths active_paths = paths;
    std::unique_ptr<Db> db;
    if (!options.dry_run) {
        active_paths = open_library(library);
        db = std::make_unique<Db>(active_paths.db_path);
    }

    Pipeline pipeline(active_paths, db.get(), profile, toolchain, options);

    int added = 0;
    int skipped = 0;
    int failed = static_cast<int>(scan.errors.size());
    int exit_code = scan.errors.empty() ? 0 : 4;

    for (const fs::path& file : files) {
        try {
            const ProcessOutcome outcome = pipeline.process_path(file);
            switch (outcome.kind) {
            case OutcomeKind::Done:
                ++added;
                break;
            case OutcomeKind::Skipped:
                ++skipped;
                break;
            case OutcomeKind::DryRun:
                ++added;
                break;
            case OutcomeKind::Failed:
                ++failed;
                exit_code = batch_exit_code(exit_code, outcome.exit_code);
                break;
            }
        } catch (const AppError& e) {
            ++failed;
            exit_code = batch_exit_code(exit_code, e.exit_code());
            std::cerr << "clipbank: " << file.string() << ": " << e.what() << "\n";
        }
    }

    if (options.dry_run) {
        std::cout << "dry-run: " << files.size() << " file(s)\n";
    } else {
        std::cout << "add summary: " << added << " added, " << skipped << " skipped, "
                  << failed << " failed\n";
    }
    return failed > 0 ? (exit_code == 0 ? 4 : exit_code) : 0;
}

nlohmann::json run_to_json(const RunSummary& summary) {
    return nlohmann::json{
        {"run_id", summary.run.run_id},
        {"source_id", summary.run.source_id},
        {"source_path", summary.source_path},
        {"profile", summary.run.profile},
        {"profile_fp", summary.run.profile_fp},
        {"state", summary.run.state},
        {"clips", summary.clip_count},
        {"duration_ms", summary.run.duration_ms},
        {"attempt", summary.run.attempt},
        {"error", summary.run.error},
        {"created_at", summary.run.created_at},
        {"updated_at", summary.run.updated_at},
    };
}

int run_ls(const std::string& library, const std::string& state_filter,
           const std::string& profile_filter, bool json_output) {
    const LibraryPaths paths = open_library(library);
    Db db(paths.db_path);
    const std::vector<RunSummary> runs = db.list_runs(state_filter, profile_filter);

    if (json_output) {
        nlohmann::json array = nlohmann::json::array();
        for (const RunSummary& summary : runs) {
            array.push_back(run_to_json(summary));
        }
        std::cout << array.dump(2) << "\n";
        return 0;
    }

    if (runs.empty()) {
        std::cout << "(no runs)\n";
        return 0;
    }

    std::cout << std::left << std::setw(14) << "RUN_ID" << std::setw(14) << "STATE"
              << std::right << std::setw(6) << "CLIPS" << "  " << std::left << std::setw(10)
              << "DURATION" << std::setw(10) << "PROFILE" << "SOURCE\n";
    for (const RunSummary& summary : runs) {
        std::cout << std::left << std::setw(14) << summary.run.run_id << std::setw(14)
                  << summary.run.state << std::right << std::setw(6) << summary.clip_count << "  "
                  << std::left << std::setw(10) << format_duration(summary.run.duration_ms)
                  << std::setw(10) << summary.run.profile << summary.source_path;
        if (!summary.run.error.empty()) {
            std::cout << "  [" << summary.run.error << "]";
        }
        std::cout << "\n";
    }
    return 0;
}

int run_manifest(const std::string& library, const std::string& out_path,
                 const std::string& run_filter) {
    const LibraryPaths paths = open_library(library);
    Db db(paths.db_path);
    const Toolchain toolchain = Toolchain::detect();
    const nlohmann::json manifest = build_manifest(paths, db, toolchain, run_filter);
    const std::string text = manifest.dump(2) + "\n";
    if (out_path.empty()) {
        std::cout << text;
    } else {
        ensure_dir(fs::path(out_path).parent_path().empty() ? fs::path(".")
                                                            : fs::path(out_path).parent_path());
        write_file_atomic(out_path, text);
        std::cout << "manifest: " << absolute_path(out_path) << "\n";
    }
    return 0;
}

int run_verify(const std::string& library, const std::string& run_filter, bool all,
               bool no_hash, bool json_output) {
    const LibraryPaths paths = open_library(library);
    ensure_ffprobe_available();
    Db db(paths.db_path);

    VerifyOptions options;
    options.no_hash = no_hash;
    if (!run_filter.empty() && !all) {
        options.run_id = run_filter;
    } else if (!run_filter.empty() && all) {
        throw UsageError("--run and --all are mutually exclusive");
    }

    const VerifySummary summary = verify_library(paths, db, options);
    if (json_output) {
        std::cout << verify_to_json(summary).dump(2) << "\n";
    } else {
        for (const VerifyRunResult& result : summary.runs) {
            std::cout << "run " << result.run_id << " ("
                      << (result.source_path.empty() ? "unknown source" : result.source_path) << "): "
                      << (result.ok ? "OK" : "FAIL") << " (" << result.clip_count << " clip(s))\n";
            for (const std::string& warning : result.warnings) {
                std::cout << "  warning: " << warning << "\n";
            }
            for (const std::string& error : result.errors) {
                std::cout << "  error: " << error << "\n";
            }
        }
        int64_t clips = 0;
        int ok_runs = 0;
        for (const VerifyRunResult& result : summary.runs) {
            clips += result.clip_count;
            ok_runs += result.ok ? 1 : 0;
        }
        std::cout << "summary: " << ok_runs << "/" << summary.runs.size() << " run(s) OK, " << clips
                  << " clip(s)\n";
    }
    return summary.ok ? 0 : 6;
}

int run_retry(const std::string& library, const std::string& run_id, bool all_failed,
              const PipelineOptions& options) {
    const LibraryPaths paths = open_library(library);
    Db db(paths.db_path);
    const Toolchain toolchain = Toolchain::detect();
    const Profile profile = Profile::detect();

    std::vector<RunRow> runs;
    if (!run_id.empty()) {
        const auto run = db.get_run(run_id);
        if (!run) {
            throw UsageError("unknown run: " + run_id);
        }
        runs.push_back(*run);
    } else if (all_failed) {
        runs = db.list_incomplete_runs();
    } else {
        throw UsageError("retry needs a run_id or --all-failed");
    }

    if (runs.empty()) {
        std::cout << "retry: nothing to do\n";
        return 0;
    }

    Pipeline pipeline(paths, &db, profile, toolchain, options);
    int done = 0;
    int failed = 0;
    int exit_code = 0;

    for (const RunRow& run : runs) {
        if (run.profile_fp != profile.fp()) {
            ++failed;
            exit_code = batch_exit_code(exit_code, 2);
            std::cerr << "clipbank: run " << run.run_id << ": profile " << run.profile
                      << " (" << run.profile_fp << ") differs from current profile ("
                      << profile.fp() << ")\n";
            continue;
        }
        const auto source = db.get_source(run.source_id);
        if (!source) {
            ++failed;
            exit_code = batch_exit_code(exit_code, 5);
            std::cerr << "clipbank: run " << run.run_id << ": source row missing\n";
            continue;
        }
        try {
            const ProcessOutcome outcome =
                pipeline.process_path(source->path, true, run.source_id);
            if (outcome.kind == OutcomeKind::Done) {
                ++done;
            } else if (outcome.kind == OutcomeKind::Skipped) {
                std::cout << "run " << run.run_id << ": skipped (" << outcome.message << ")\n";
            } else {
                ++failed;
                exit_code = batch_exit_code(exit_code, outcome.exit_code);
            }
        } catch (const AppError& e) {
            ++failed;
            exit_code = batch_exit_code(exit_code, e.exit_code());
            std::cerr << "clipbank: run " << run.run_id << ": " << e.what() << "\n";
        }
    }

    std::cout << "retry summary: " << done << " done, " << failed << " failed\n";
    return failed > 0 ? (exit_code == 0 ? 5 : exit_code) : 0;
}

} // namespace

int main(int argc, char** argv) {
    CLI::App app{"ClipBank " CLIPBANK_VERSION " - turn arbitrary video into catalogued clips"};
    app.set_version_flag("-v,--version", std::string("clipbank ") + CLIPBANK_VERSION);
    app.require_subcommand(1);

    std::string library = default_library();

    auto add_library_option = [&](CLI::App* sub) {
        sub->add_option("-L,--library", library,
                        "library directory (default: $CLIPBANK_LIBRARY or ./library)")
            ->capture_default_str();
    };

    auto* init_cmd = app.add_subcommand("init", "create the library directory and database");
    add_library_option(init_cmd);

    auto* add_cmd = app.add_subcommand("add", "probe, normalize, segment and catalogue videos");
    add_library_option(add_cmd);
    std::vector<std::string> add_inputs;
    std::string add_profile = "detect";
    bool dry_run = false;
    bool no_hash = false;
    bool force_reencode = false;
    add_cmd->add_option("paths", add_inputs, "video files or directories (recursive)")
        ->required()
        ->expected(-1);
    add_cmd
        ->add_option("--profile", add_profile, "processing profile (only detect in v0.1.0)")
        ->check(CLI::IsMember(std::vector<std::string>{"detect"}))
        ->capture_default_str();
    add_cmd->add_flag("--dry-run", dry_run, "probe and print the plan without writing anything");
    add_cmd->add_flag("--no-hash", no_hash, "skip SHA-256 of normalized file and clips");
    add_cmd->add_flag("--force-reencode", force_reencode, "discard derived artifacts and re-encode");

    auto* ls_cmd = app.add_subcommand("ls", "list runs in the library");
    add_library_option(ls_cmd);
    std::string ls_state;
    std::string ls_profile;
    bool ls_json = false;
    ls_cmd->add_option("--state", ls_state, "filter by run state");
    ls_cmd->add_option("--profile", ls_profile, "filter by profile");
    ls_cmd->add_flag("--json", ls_json, "machine-readable output");

    auto* manifest_cmd = app.add_subcommand("manifest", "emit manifest schema v1");
    add_library_option(manifest_cmd);
    std::string manifest_out;
    std::string manifest_run;
    manifest_cmd->add_option("-o,--out", manifest_out, "write to a file instead of stdout");
    manifest_cmd->add_option("--run", manifest_run, "restrict to one done run");

    auto* verify_cmd = app.add_subcommand("verify", "check specs, durations, frames, keyframes and hashes");
    add_library_option(verify_cmd);
    std::string verify_run;
    bool verify_all = false;
    bool verify_no_hash = false;
    bool verify_json = false;
    verify_cmd->add_option("--run", verify_run, "verify one run");
    verify_cmd->add_flag("--all", verify_all, "verify every done run (default)");
    verify_cmd->add_flag("--no-hash", verify_no_hash, "skip SHA-256 checks");
    verify_cmd->add_flag("--json", verify_json, "machine-readable output");

    auto* retry_cmd = app.add_subcommand("retry", "resume failed or interrupted runs");
    add_library_option(retry_cmd);
    std::string retry_run;
    bool retry_all_failed = false;
    bool retry_no_hash = false;
    bool retry_force = false;
    retry_cmd->add_option("run_id", retry_run, "run to retry");
    retry_cmd->add_flag("--all-failed", retry_all_failed, "retry every run not in state done");
    retry_cmd->add_flag("--no-hash", retry_no_hash, "skip SHA-256");
    retry_cmd->add_flag("--force-reencode", retry_force, "discard derived artifacts and re-encode");

    try {
        app.parse(argc, argv);
    } catch (const CLI::CallForHelp&) {
        std::cout << app.help();
        return 0;
    } catch (const CLI::CallForVersion&) {
        std::cout << "clipbank " CLIPBANK_VERSION "\n";
        return 0;
    } catch (const CLI::ParseError& e) {
        std::cerr << "clipbank: " << e.what() << "\n";
        std::cerr << "Try 'clipbank --help' for more information.\n";
        return 2;
    }

    try {
        if (app.got_subcommand(init_cmd)) {
            return run_init(library);
        }
        if (app.got_subcommand(add_cmd)) {
            PipelineOptions options;
            options.dry_run = dry_run;
            options.no_hash = no_hash;
            options.force_reencode = force_reencode;
            return run_add(library, add_inputs, add_profile, options);
        }
        if (app.got_subcommand(ls_cmd)) {
            return run_ls(library, ls_state, ls_profile, ls_json);
        }
        if (app.got_subcommand(manifest_cmd)) {
            return run_manifest(library, manifest_out, manifest_run);
        }
        if (app.got_subcommand(verify_cmd)) {
            return run_verify(library, verify_run, verify_all, verify_no_hash, verify_json);
        }
        if (app.got_subcommand(retry_cmd)) {
            PipelineOptions options;
            options.no_hash = retry_no_hash;
            options.force_reencode = retry_force;
            return run_retry(library, retry_run, retry_all_failed, options);
        }
    } catch (const AppError& e) {
        std::cerr << "clipbank: " << e.what() << "\n";
        return e.exit_code();
    } catch (const std::exception& e) {
        std::cerr << "clipbank: internal error: " << e.what() << "\n";
        return 5;
    }

    std::cerr << app.help();
    return 2;
}
