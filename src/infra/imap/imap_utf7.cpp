#include "mailfs/infra/imap/imap_utf7.hpp"

#include <codecvt>
#include <cstdint>
#include <locale>
#include <stdexcept>
#include <string>
#include <vector>

namespace mailfs::infra::imap {

namespace {

constexpr char kBase64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+,";

bool is_direct_char(unsigned char ch) {
  return ch >= 0x20 && ch <= 0x7e && ch != '&';
}

std::string utf16_to_utf8(const std::u16string& text) {
  std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t> converter;
  return converter.to_bytes(text);
}

std::u16string utf8_to_utf16(std::string_view text) {
  std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t> converter;
  return converter.from_bytes(text.data(), text.data() + text.size());
}

std::string encode_modified_base64(const std::vector<unsigned char>& bytes) {
  std::string out;
  std::size_t i = 0;
  while (i + 3 <= bytes.size()) {
    const std::uint32_t block =
        (static_cast<std::uint32_t>(bytes[i]) << 16) |
        (static_cast<std::uint32_t>(bytes[i + 1]) << 8) |
        static_cast<std::uint32_t>(bytes[i + 2]);
    out.push_back(kBase64Alphabet[(block >> 18) & 0x3f]);
    out.push_back(kBase64Alphabet[(block >> 12) & 0x3f]);
    out.push_back(kBase64Alphabet[(block >> 6) & 0x3f]);
    out.push_back(kBase64Alphabet[block & 0x3f]);
    i += 3;
  }

  const auto remaining = bytes.size() - i;
  if (remaining == 1) {
    const std::uint32_t block = static_cast<std::uint32_t>(bytes[i]) << 16;
    out.push_back(kBase64Alphabet[(block >> 18) & 0x3f]);
    out.push_back(kBase64Alphabet[(block >> 12) & 0x3f]);
  } else if (remaining == 2) {
    const std::uint32_t block =
        (static_cast<std::uint32_t>(bytes[i]) << 16) |
        (static_cast<std::uint32_t>(bytes[i + 1]) << 8);
    out.push_back(kBase64Alphabet[(block >> 18) & 0x3f]);
    out.push_back(kBase64Alphabet[(block >> 12) & 0x3f]);
    out.push_back(kBase64Alphabet[(block >> 6) & 0x3f]);
  }

  return out;
}

std::vector<unsigned char> decode_modified_base64(std::string_view text) {
  auto decode_char = [](char ch) -> int {
    if (ch >= 'A' && ch <= 'Z') {
      return ch - 'A';
    }
    if (ch >= 'a' && ch <= 'z') {
      return ch - 'a' + 26;
    }
    if (ch >= '0' && ch <= '9') {
      return ch - '0' + 52;
    }
    if (ch == '+') {
      return 62;
    }
    if (ch == ',') {
      return 63;
    }
    return -1;
  };

  std::vector<unsigned char> bytes;
  int accumulator = 0;
  int bits = 0;
  for (const auto ch : text) {
    const auto value = decode_char(ch);
    if (value < 0) {
      throw std::runtime_error("invalid modified UTF-7 base64 character");
    }

    accumulator = (accumulator << 6) | value;
    bits += 6;
    while (bits >= 8) {
      bits -= 8;
      bytes.push_back(static_cast<unsigned char>((accumulator >> bits) & 0xff));
    }
  }

  if (bits != 0) {
    const auto mask = (1 << bits) - 1;
    if ((accumulator & mask) != 0) {
      throw std::runtime_error("invalid modified UTF-7 padding");
    }
  }

  return bytes;
}

std::string encode_utf16_segment(const std::u16string& segment) {
  std::vector<unsigned char> bytes;
  bytes.reserve(segment.size() * 2);
  for (const auto code_unit : segment) {
    bytes.push_back(static_cast<unsigned char>((code_unit >> 8) & 0xff));
    bytes.push_back(static_cast<unsigned char>(code_unit & 0xff));
  }
  return "&" + encode_modified_base64(bytes) + "-";
}

std::string decode_utf16_segment(std::string_view segment) {
  if (segment.empty()) {
    return "&";
  }

  const auto bytes = decode_modified_base64(segment);
  if (bytes.size() % 2 != 0) {
    throw std::runtime_error("invalid modified UTF-7 UTF-16 byte count");
  }

  std::u16string utf16;
  utf16.reserve(bytes.size() / 2);
  for (std::size_t i = 0; i < bytes.size(); i += 2) {
    utf16.push_back(static_cast<char16_t>((bytes[i] << 8) | bytes[i + 1]));
  }
  return utf16_to_utf8(utf16);
}

}  // namespace

std::string decode_imap_utf7(std::string_view text) {
  std::string decoded;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const auto ch = text[i];
    if (ch != '&') {
      decoded.push_back(ch);
      continue;
    }

    const auto end = text.find('-', i);
    if (end == std::string_view::npos) {
      decoded.append(text.substr(i));
      break;
    }

    try {
      decoded += decode_utf16_segment(text.substr(i + 1, end - i - 1));
    } catch (...) {
      decoded.append(text.substr(i, end - i + 1));
    }
    i = end;
  }
  return decoded;
}

std::string encode_imap_utf7(std::string_view text) {
  std::string encoded;
  std::u16string pending;

  const auto flush_pending = [&]() {
    if (!pending.empty()) {
      encoded += encode_utf16_segment(pending);
      pending.clear();
    }
  };

  const auto utf16 = utf8_to_utf16(text);
  for (const auto code_unit : utf16) {
    if (code_unit <= 0x7f && is_direct_char(static_cast<unsigned char>(code_unit))) {
      flush_pending();
      encoded.push_back(static_cast<char>(code_unit));
      continue;
    }
    if (code_unit == u'&') {
      flush_pending();
      encoded += "&-";
      continue;
    }
    pending.push_back(code_unit);
  }

  flush_pending();
  return encoded;
}

}  // namespace mailfs::infra::imap
