#include "mailfs/core/security/xor_codec.hpp"

#include <array>
#include <stdexcept>

namespace mailfs::core::security {

namespace {

constexpr std::array<unsigned char, 12> kXorKey = {
    0xa3, 0x5f, 0xe1, 0x7c, 0x92, 0x4b, 0xd8, 0x06, 0xf7, 0x31, 0x6e, 0xb4};

std::string xor_transform(const std::string& value) {
  std::string out = value;
  for (std::size_t i = 0; i < value.size(); ++i) {
    out[i] = static_cast<char>(static_cast<unsigned char>(value[i]) ^ kXorKey[i % kXorKey.size()]);
  }
  return out;
}

std::string to_hex(const std::string& value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(value.size() * 2);
  for (unsigned char ch : value) {
    out.push_back(kHex[(ch >> 4) & 0x0f]);
    out.push_back(kHex[ch & 0x0f]);
  }
  return out;
}

std::string from_hex(const std::string& value) {
  if (value.size() % 2 != 0) {
    throw std::runtime_error("invalid hex string");
  }

  auto decode_nibble = [](char ch) -> unsigned char {
    if (ch >= '0' && ch <= '9') {
      return static_cast<unsigned char>(ch - '0');
    }
    if (ch >= 'a' && ch <= 'f') {
      return static_cast<unsigned char>(ch - 'a' + 10);
    }
    if (ch >= 'A' && ch <= 'F') {
      return static_cast<unsigned char>(ch - 'A' + 10);
    }
    throw std::runtime_error("invalid hex digit");
  };

  std::string out;
  out.reserve(value.size() / 2);
  for (std::size_t i = 0; i < value.size(); i += 2) {
    const auto high = decode_nibble(value[i]);
    const auto low = decode_nibble(value[i + 1]);
    out.push_back(static_cast<char>((high << 4) | low));
  }
  return out;
}

}  // namespace

std::string encrypt_string(const std::string& value) {
  if (value.empty()) {
    return value;
  }
  return to_hex(xor_transform(value));
}

std::string decrypt_string(const std::string& value) {
  if (value.empty()) {
    return value;
  }
  try {
    return xor_transform(from_hex(value));
  } catch (...) {
    return value;
  }
}

bool should_encrypt_mailbox(const std::string& mailbox) {
  if (mailbox.empty()) {
    return false;
  }
  const auto last_pos = mailbox.find_last_of("/\\");
  const auto tail = last_pos == std::string::npos ? mailbox : mailbox.substr(last_pos + 1);
  return !tail.empty() && tail.front() == '.';
}

}  // namespace mailfs::core::security
