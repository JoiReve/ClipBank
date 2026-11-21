#include "clipbank/subprocess.hpp"

#include "clipbank/errors.hpp"
#include "clipbank/util.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace cb {
namespace {

void close_fd(int& fd) {
  if (fd >= 0) {
    ::close(fd);
    fd = -1;
  }
}

void append_tail(std::string& tail, const char* data, std::size_t len, std::size_t limit) {
  tail.append(data, len);
  if (tail.size() > limit) {
    tail.erase(0, tail.size() - limit);
  }
}

}  // namespace

ProcessResult run_process(const ProcessOptions& options) {
  ProcessResult result;
  if (options.args.empty()) {
    result.error = "empty command";
    return result;
  }

  int out_pipe[2] = {-1, -1};
  int err_pipe[2] = {-1, -1};
  if (::pipe2(out_pipe, O_CLOEXEC) != 0 || ::pipe2(err_pipe, O_CLOEXEC) != 0) {
    close_fd(out_pipe[0]);
    close_fd(out_pipe[1]);
    close_fd(err_pipe[0]);
    close_fd(err_pipe[1]);
    throw InternalError(std::string("pipe2 failed: ") + std::strerror(errno));
  }

  const int64_t start_ms = unix_ms_now();
  const pid_t pid = ::fork();
  if (pid < 0) {
    close_fd(out_pipe[0]);
    close_fd(out_pipe[1]);
    close_fd(err_pipe[0]);
    close_fd(err_pipe[1]);
    throw InternalError(std::string("fork failed: ") + std::strerror(errno));
  }

  if (pid == 0) {
    // Child: own process group so a timeout can kill ffmpeg and its helpers;
    // die with clipbank so a SIGKILL of the parent does not orphan ffmpeg.
    ::prctl(PR_SET_PDEATHSIG, SIGKILL);
    ::setpgid(0, 0);
    ::dup2(out_pipe[1], STDOUT_FILENO);
    ::dup2(err_pipe[1], STDERR_FILENO);
    ::close(out_pipe[0]);
    ::close(out_pipe[1]);
    ::close(err_pipe[0]);
    ::close(err_pipe[1]);

    std::vector<char*> argv;
    argv.reserve(options.args.size() + 1);
    for (const std::string& arg : options.args) {
      argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

    ::execvp(argv[0], argv.data());
    const int saved_errno = errno;
    ::dprintf(STDERR_FILENO, "clipbank: cannot execute %s: %s\n",
              options.args[0].c_str(), std::strerror(saved_errno));
    ::_exit(127);
  }

  result.started = true;
  // Close the race where a timeout could fire before the child runs setpgid().
  ::setpgid(pid, pid);
  ::close(out_pipe[1]);
  out_pipe[1] = -1;
  ::close(err_pipe[1]);
  err_pipe[1] = -1;

  ::fcntl(out_pipe[0], F_SETFL, ::fcntl(out_pipe[0], F_GETFL) | O_NONBLOCK);
  ::fcntl(err_pipe[0], F_SETFL, ::fcntl(err_pipe[0], F_GETFL) | O_NONBLOCK);

  const int64_t deadline = options.timeout.count() > 0 ? start_ms + options.timeout.count() : 0;
  int64_t kill_deadline = 0;
  bool killed = false;

  std::string out_linebuf;
  std::string err_linebuf;

  auto emit_lines = [](std::string& linebuf, const std::function<void(const std::string&)>& callback,
                       bool flush_remainder) {
    std::size_t pos = 0;
    while ((pos = linebuf.find('\n')) != std::string::npos) {
      std::string line = linebuf.substr(0, pos);
      linebuf.erase(0, pos + 1);
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      if (callback) {
        callback(line);
      }
    }
    if (flush_remainder && !linebuf.empty()) {
      if (callback) {
        callback(linebuf);
      }
      linebuf.clear();
    }
    // Guard against a pathological stream with no newlines.
    if (linebuf.size() > 1024 * 1024) {
      linebuf.erase(0, linebuf.size() - 65536);
    }
  };

  auto drain_fd = [&](int fd, bool is_stderr, std::string& linebuf) -> bool {
    char buffer[65536];
    for (;;) {
      const ssize_t n = ::read(fd, buffer, sizeof(buffer));
      if (n > 0) {
        if (is_stderr) {
          append_tail(result.stderr_tail, buffer, static_cast<std::size_t>(n),
                      options.stderr_tail_limit);
          if (options.on_stderr_line) {
            linebuf.append(buffer, static_cast<std::size_t>(n));
            emit_lines(linebuf, options.on_stderr_line, false);
          }
        } else {
          if (options.capture_stdout && result.stdout_text.size() < options.stdout_limit) {
            const std::size_t room = options.stdout_limit - result.stdout_text.size();
            result.stdout_text.append(buffer, std::min<std::size_t>(room, static_cast<std::size_t>(n)));
          }
          if (options.on_stdout_line) {
            linebuf.append(buffer, static_cast<std::size_t>(n));
            emit_lines(linebuf, options.on_stdout_line, false);
          }
        }
        continue;
      }
      if (n == 0) {
        emit_lines(linebuf, is_stderr ? options.on_stderr_line : options.on_stdout_line, true);
        return false;
      }
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return true;
      }
      emit_lines(linebuf, is_stderr ? options.on_stderr_line : options.on_stdout_line, true);
      return false;
    }
  };

  bool out_open = true;
  bool err_open = true;

  while (out_open || err_open) {
    const int64_t now = unix_ms_now();
    if (!killed && deadline > 0 && now >= deadline) {
      if (::kill(-pid, SIGKILL) != 0 && errno != ESRCH) {
        result.error = std::string("kill failed: ") + std::strerror(errno);
      }
      if (::kill(pid, SIGKILL) != 0 && errno != ESRCH) {
        // The group kill is the normal path; a direct kill is a fallback.
      }
      killed = true;
      result.timed_out = true;
      kill_deadline = now + 5000;
    }
    if (killed && kill_deadline > 0 && now >= kill_deadline) {
      break;
    }

    struct pollfd fds[2];
    int nfds = 0;
    int out_slot = -1;
    int err_slot = -1;
    if (out_open) {
      out_slot = nfds;
      fds[nfds].fd = out_pipe[0];
      fds[nfds].events = POLLIN;
      fds[nfds].revents = 0;
      ++nfds;
    }
    if (err_open) {
      err_slot = nfds;
      fds[nfds].fd = err_pipe[0];
      fds[nfds].events = POLLIN;
      fds[nfds].revents = 0;
      ++nfds;
    }
    if (nfds == 0) {
      break;
    }

    int poll_ms = 100;
    if (!killed && deadline > 0) {
      poll_ms = static_cast<int>(std::clamp<int64_t>(deadline - now, 0, 100));
    }
    const int rc = ::poll(fds, static_cast<nfds_t>(nfds), poll_ms);
    if (rc < 0 && errno != EINTR) {
      break;
    }
    if (rc <= 0) {
      continue;
    }

    if (out_open && out_slot >= 0 && fds[out_slot].revents != 0) {
      if (!drain_fd(out_pipe[0], false, out_linebuf)) {
        out_open = false;
      }
    }
    if (err_open && err_slot >= 0 && fds[err_slot].revents != 0) {
      if (!drain_fd(err_pipe[0], true, err_linebuf)) {
        err_open = false;
      }
    }
  }

  close_fd(out_pipe[0]);
  close_fd(err_pipe[0]);

  int status = 0;
  pid_t waited = 0;
  bool reaped = false;

  // Even if the child closed its pipes early, the timeout must still apply.
  while (true) {
    waited = ::waitpid(pid, &status, WNOHANG);
    if (waited == pid) {
      reaped = true;
      break;
    }
    if (waited < 0 && errno != EINTR) {
      break;
    }

    const int64_t now = unix_ms_now();
    if (!killed && deadline > 0 && now >= deadline) {
      if (::kill(-pid, SIGKILL) != 0 && errno != ESRCH) {
        result.error = std::string("kill failed: ") + std::strerror(errno);
      }
      ::kill(pid, SIGKILL);
      killed = true;
      result.timed_out = true;
      kill_deadline = now + 5000;
      continue;
    }
    if (killed && kill_deadline > 0 && now >= kill_deadline) {
      break;
    }
    if (deadline == 0 && !killed) {
      // No timeout was requested: wait for normal exit.
      do {
        waited = ::waitpid(pid, &status, 0);
      } while (waited < 0 && errno == EINTR);
      reaped = waited == pid;
      break;
    }
    ::usleep(10000);
  }

  if (!reaped && waited != pid) {
    // SIGKILL normally reaps immediately; this only waits for kernel cleanup.
    do {
      waited = ::waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
  }

  result.elapsed_ms = unix_ms_now() - start_ms;
  if (waited == pid) {
    if (WIFEXITED(status)) {
      result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
      result.signalled = true;
      result.term_signal = WTERMSIG(status);
      result.exit_code = 128 + result.term_signal;
    }
  } else if (result.error.empty()) {
    result.error = std::string("waitpid failed: ") + std::strerror(errno);
  }

  if (result.timed_out && result.error.empty()) {
    result.error = "timed out after " + std::to_string(options.timeout.count()) + "ms";
  }
  return result;
}

}  // namespace cb
