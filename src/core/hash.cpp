#include "mailfs/core/hash.hpp"

#include <fstream>
#include <stdexcept>

#include <mbedtls/md5.h>

namespace mailfs::core {

namespace {

std::string to_hex_string(const unsigned char* bytes, std::size_t size) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string text;
  text.reserve(size * 2);
  for (std::size_t i = 0; i < size; ++i) {
    text.push_back(kHex[(bytes[i] >> 4) & 0x0f]);
    text.push_back(kHex[bytes[i] & 0x0f]);
  }
  return text;
}

}  // namespace

std::string md5_hex(const std::vector<unsigned char>& bytes) {
  unsigned char digest[16];
  if (mbedtls_md5(bytes.data(), bytes.size(), digest) != 0) {
    throw std::runtime_error("mbedtls_md5 failed");
  }
  return to_hex_string(digest, sizeof(digest));
}

std::string md5_hex(const std::filesystem::path& file_path) {
  std::ifstream input(file_path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("failed to open file for md5: " + file_path.u8string());
  }

  mbedtls_md5_context ctx;
  mbedtls_md5_init(&ctx);
  if (mbedtls_md5_starts(&ctx) != 0) {
    mbedtls_md5_free(&ctx);
    throw std::runtime_error("mbedtls_md5_starts failed");
  }

  std::vector<char> buffer(64 * 1024);
  while (input.good()) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto read_count = input.gcount();
    if (read_count > 0) {
      if (mbedtls_md5_update(&ctx,
                             reinterpret_cast<const unsigned char*>(buffer.data()),
                             static_cast<std::size_t>(read_count)) != 0) {
        mbedtls_md5_free(&ctx);
        throw std::runtime_error("mbedtls_md5_update failed");
      }
    }
  }

  unsigned char digest[16];
  if (mbedtls_md5_finish(&ctx, digest) != 0) {
    mbedtls_md5_free(&ctx);
    throw std::runtime_error("mbedtls_md5_finish failed");
  }
  mbedtls_md5_free(&ctx);
  return to_hex_string(digest, sizeof(digest));
}

}  // namespace mailfs::core
