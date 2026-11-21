#include "clipbank/util.hpp"

#include "clipbank/errors.hpp"
#include "clipbank/sha256.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>

#include <sys/stat.h>
#include <unistd.h>

namespace cb {
namespace {

constexpr std::array<const char*, 20> kVideoExtensions = {
    ".mp4", ".m4v", ".mkv", ".mov", ".avi", ".webm", ".ts", ".mts", ".m2ts", ".flv",
    ".wmv", ".mpg", ".mpeg", ".mxf", ".3gp", ".ogv", ".vob", ".rmvb", ".asf", ".divx",
};

std::string offset_string(const std::tm& tm) {
  char buf[8] = {};
  if (std::strftime(buf, sizeof(buf), "%z", &tm) == 0) {
    return "+00:00";
  }
  // %z is normally +HHMM; insert the colon required by RFC 3339.
  std::string off(buf);
  if (off.size() == 5) {
    off.insert(3, ":");
  }
  return off;
}

}  // namespace

FileStat stat_file(const fs::path& path) {
  struct stat st {};
  if (::stat(path.c_str(), &st) != 0) {
    throw InputError("cannot stat " + path.string() + ": " + std::strerror(errno));
  }
  FileStat out;
  out.size = static_cast<int64_t>(st.st_size);
  out.mtime_ns = static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000LL +
                 static_cast<int64_t>(st.st_mtim.tv_nsec);
  return out;
}

int64_t unix_ms_now() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

int64_t deadline_after_ms(int64_t milliseconds) {
  return unix_ms_now() + milliseconds;
}

bool deadline_expired(int64_t deadline_ms) {
  return deadline_ms > 0 && unix_ms_now() > deadline_ms;
}

std::string now_iso8601() {
  const std::time_t now = std::time(nullptr);
  std::tm tm {};
  localtime_r(&now, &tm);
  char date[32] = {};
  std::strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", &tm);
  return std::string(date) + offset_string(tm);
}

std::string format_duration(int64_t milliseconds) {
  if (milliseconds < 0) {
    milliseconds = 0;
  }
  std::ostringstream oss;
  if (milliseconds < 1000) {
    oss << milliseconds << "ms";
  } else if (milliseconds < 60000) {
    oss << std::fixed << std::setprecision(2) << (static_cast<double>(milliseconds) / 1000.0) << "s";
  } else {
    const int64_t minutes = milliseconds / 60000;
    const double seconds = static_cast<double>(milliseconds % 60000) / 1000.0;
    oss << minutes << "m" << std::fixed << std::setprecision(1) << seconds << "s";
  }
  return oss.str();
}

std::string human_bytes(int64_t bytes) {
  static constexpr const char* kUnits[] = {"B", "KiB", "MiB", "GiB", "TiB"};
  double value = static_cast<double>(bytes);
  int unit = 0;
  while (value >= 1024.0 && unit < 4) {
    value /= 1024.0;
    ++unit;
  }
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(unit == 0 ? 0 : 1) << value << " " << kUnits[unit];
  return oss.str();
}

std::string quote_arg(const std::string& arg) {
  if (arg.empty()) {
    return "''";
  }
  bool safe = true;
  for (char c : arg) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' ||
                    c == '/' || c == ':' || c == '=' || c == ',' || c == '+';
    if (!ok) {
      safe = false;
      break;
    }
  }
  if (safe) {
    return arg;
  }
  std::string out = "'";
  for (char c : arg) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out.push_back(c);
    }
  }
  out.push_back('\'');
  return out;
}

std::string join_command(const std::vector<std::string>& args) {
  std::string out;
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (i != 0) {
      out.push_back(' ');
    }
    out += quote_arg(args[i]);
  }
  return out;
}

std::string to_lower(std::string text) {
  for (char& c : text) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return text;
}

bool ends_with_ci(const std::string& text, const std::string& suffix) {
  if (suffix.size() > text.size()) {
    return false;
  }
  return to_lower(text.substr(text.size() - suffix.size())) == to_lower(suffix);
}

std::string trim(std::string text) {
  const auto is_space = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  std::size_t begin = 0;
  while (begin < text.size() && is_space(static_cast<unsigned char>(text[begin]))) {
    ++begin;
  }
  std::size_t end = text.size();
  while (end > begin && is_space(static_cast<unsigned char>(text[end - 1]))) {
    --end;
  }
  return text.substr(begin, end - begin);
}

bool is_video_extension(const fs::path& path) {
  const std::string ext = to_lower(path.extension().string());
  for (const char* candidate : kVideoExtensions) {
    if (ext == candidate) {
      return true;
    }
  }
  return false;
}

std::string read_file(const fs::path& path, std::size_t max_bytes) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw InputError("cannot open " + path.string());
  }
  std::string content;
  content.resize(max_bytes);
  in.read(content.data(), static_cast<std::streamsize>(max_bytes));
  content.resize(static_cast<std::size_t>(in.gcount()));
  return content;
}

void write_file_atomic(const fs::path& path, const std::string& content) {
  const fs::path tmp = path.string() + ".tmp." + std::to_string(::getpid());
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      throw InternalError("cannot write " + tmp.string());
    }
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.flush();
    if (!out) {
      throw InternalError("failed writing " + tmp.string());
    }
  }
  std::error_code ec;
  fs::rename(tmp, path, ec);
  if (ec) {
    fs::remove(tmp, ec);
    throw InternalError("cannot rename " + tmp.string() + " -> " + path.string());
  }
}

void ensure_dir(const fs::path& path) {
  std::error_code ec;
  fs::create_directories(path, ec);
  if (ec) {
    throw InternalError("cannot create directory " + path.string() + ": " + ec.message());
  }
}

std::string absolute_path(const fs::path& path) {
  std::error_code ec;
  const fs::path abs = fs::absolute(path, ec);
  if (ec) {
    return path.string();
  }
  const fs::path canonical = fs::weakly_canonical(abs, ec);
  return ec ? abs.string() : canonical.string();
}

std::string relative_path_string(const fs::path& path, const fs::path& base) {
  std::error_code ec;
  const fs::path rel = fs::relative(path, base, ec);
  if (ec) {
    return path.string();
  }
  return rel.generic_string();
}

std::string sha256_file(const fs::path& path, int64_t deadline_ms) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw InputError("cannot open " + path.string() + " for hashing");
  }

  Sha256 sha;
  std::array<char, 1024 * 1024> buffer{};
  while (in) {
    in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize got = in.gcount();
    if (got > 0) {
      sha.update(buffer.data(), static_cast<std::size_t>(got));
    }
    if (deadline_expired(deadline_ms)) {
      throw InternalError("hashing timed out: " + path.string());
    }
  }
  if (!in.eof()) {
    throw InputError("error reading " + path.string() + " for hashing");
  }
  const auto digest = sha.final();
  return to_hex(digest.data(), digest.size());
}

}  // namespace cb
