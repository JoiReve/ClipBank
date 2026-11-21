#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace cb {

// Incremental SHA-256 (FIPS 180-4). Single-use final().
class Sha256 {
 public:
  Sha256();

  void update(const void* data, std::size_t len);
  void update(std::string_view text) { update(text.data(), text.size()); }

  // Finalizes the digest. Calling final() more than once is an error.
  std::array<std::uint8_t, 32> final();

  static std::string hex_digest(std::string_view text);

 private:
  void transform(const std::uint8_t* block);

  std::uint32_t state_[8];
  std::uint64_t total_len_ = 0;
  std::uint8_t buffer_[64] = {};
  std::size_t buffer_len_ = 0;
  bool finalized_ = false;
};

std::string to_hex(const std::uint8_t* data, std::size_t len);

}  // namespace cb
