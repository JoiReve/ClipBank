#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace cb {

struct ProcessOptions {
    std::vector<std::string> args;        // args[0] is the executable, PATH-resolved via execvp.
    std::chrono::milliseconds timeout{0}; // 0 = no timeout
    bool capture_stdout = false;
    std::size_t stdout_limit = 16u * 1024u * 1024u;
    std::size_t stderr_tail_limit = 8192;
    std::function<void(const std::string&)> on_stdout_line;
    std::function<void(const std::string&)> on_stderr_line;
};

struct ProcessResult {
    bool started = false;
    bool timed_out = false;
    bool signalled = false;
    int exit_code = -1;
    int term_signal = 0;
    int64_t elapsed_ms = 0;
    std::string stdout_text;
    std::string stderr_tail;
    std::string error;

    bool ok() const { return started && !timed_out && !signalled && exit_code == 0; }
};

// fork/execvp + pipes. On timeout the whole child process group is killed
// with SIGKILL; stderr keeps only the tail. Never goes through a shell.
ProcessResult run_process(const ProcessOptions& options);

} // namespace cb
