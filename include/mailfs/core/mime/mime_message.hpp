#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace mailfs::core::mime {

struct MimePart {
  std::map<std::string, std::string> headers;
  std::vector<std::uint8_t> body;
};

class MimeMessage {
 public:
  std::map<std::string, std::string> headers;
  std::vector<MimePart> parts;

  [[nodiscard]] std::string render_multipart_mixed(const std::string& boundary) const;
  static MimeMessage parse(const std::string& raw_message);
};

std::string make_boundary();
std::string encode_quoted_printable(const std::vector<std::uint8_t>& bytes);
std::vector<std::uint8_t> decode_quoted_printable(const std::string& text);

}  // namespace mailfs::core::mime
