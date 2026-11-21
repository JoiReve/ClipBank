#include "clipbank/profile.hpp"

#include "clipbank/errors.hpp"
#include "clipbank/sha256.hpp"

#include <iomanip>
#include <sstream>

namespace cb {

Profile Profile::detect() {
  return Profile{};
}

nlohmann::json Profile::params() const {
  return nlohmann::json{
      {"width", width},
      {"height", height},
      {"fps_num", fps_num},
      {"fps_den", fps_den},
      {"codec", codec},
      {"crf", crf},
      {"preset", preset},
      {"pix_fmt", pix_fmt},
      {"segment_seconds", segment_seconds},
      {"audio", audio},
  };
}

nlohmann::json Profile::to_json() const {
  nlohmann::json json = params();
  json["name"] = name;
  return json;
}

std::string Profile::canonical() const {
  return to_json().dump();
}

std::string Profile::fp() const {
  return Sha256::hex_digest(canonical()).substr(0, 8);
}

std::string make_source_id(const std::string& sha256_hex) {
  if (sha256_hex.size() < 12) {
    throw InternalError("source sha256 is too short");
  }
  return sha256_hex.substr(0, 12);
}

std::string make_run_id(const std::string& source_id, const std::string& profile_fp) {
  return Sha256::hex_digest(source_id + "|" + profile_fp).substr(0, 12);
}

std::string make_clip_id(const std::string& run_id, int64_t index) {
  std::ostringstream oss;
  oss << run_id << '-' << std::setw(3) << std::setfill('0') << index;
  return oss.str();
}

}  // namespace cb
