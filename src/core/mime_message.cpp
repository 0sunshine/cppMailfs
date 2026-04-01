#include "mailfs/core/mime/mime_message.hpp"

#include <array>
#include <cctype>
#include <chrono>
#include <random>
#include <sstream>
#include <stdexcept>

namespace mailfs::core::mime {

namespace {

std::string trim(std::string value) {
  while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ' || value.back() == '\t')) {
    value.pop_back();
  }
  std::size_t start = 0;
  while (start < value.size() && (value[start] == ' ' || value[start] == '\t')) {
    ++start;
  }
  return value.substr(start);
}

std::string base64_encode(const std::vector<std::uint8_t>& bytes) {
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  std::string out;
  out.reserve(((bytes.size() + 2) / 3) * 4);

  for (std::size_t i = 0; i < bytes.size(); i += 3) {
    const std::uint32_t b0 = bytes[i];
    const std::uint32_t b1 = (i + 1 < bytes.size()) ? bytes[i + 1] : 0;
    const std::uint32_t b2 = (i + 2 < bytes.size()) ? bytes[i + 2] : 0;
    const std::uint32_t block = (b0 << 16) | (b1 << 8) | b2;

    out.push_back(kAlphabet[(block >> 18) & 0x3f]);
    out.push_back(kAlphabet[(block >> 12) & 0x3f]);
    out.push_back(i + 1 < bytes.size() ? kAlphabet[(block >> 6) & 0x3f] : '=');
    out.push_back(i + 2 < bytes.size() ? kAlphabet[block & 0x3f] : '=');
  }

  return out;
}

std::vector<std::uint8_t> base64_decode(const std::string& text) {
  static const std::array<int, 256> kLookup = [] {
    std::array<int, 256> table{};
    table.fill(-1);
    const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (std::size_t i = 0; i < alphabet.size(); ++i) {
      table[static_cast<unsigned char>(alphabet[i])] = static_cast<int>(i);
    }
    table[static_cast<unsigned char>('=')] = 0;
    return table;
  }();

  std::string compact;
  compact.reserve(text.size());
  for (const auto ch : text) {
    if (ch != '\r' && ch != '\n' && ch != ' ' && ch != '\t') {
      compact.push_back(ch);
    }
  }

  if (compact.size() % 4 != 0) {
    throw std::runtime_error("invalid base64 payload");
  }

  std::vector<std::uint8_t> out;
  out.reserve((compact.size() / 4) * 3);

  for (std::size_t i = 0; i < compact.size(); i += 4) {
    const int c0 = kLookup[static_cast<unsigned char>(compact[i])];
    const int c1 = kLookup[static_cast<unsigned char>(compact[i + 1])];
    const int c2 = kLookup[static_cast<unsigned char>(compact[i + 2])];
    const int c3 = kLookup[static_cast<unsigned char>(compact[i + 3])];
    if (c0 < 0 || c1 < 0 || c2 < 0 || c3 < 0) {
      throw std::runtime_error("invalid base64 alphabet");
    }

    const std::uint32_t block = (static_cast<std::uint32_t>(c0) << 18) |
                                (static_cast<std::uint32_t>(c1) << 12) |
                                (static_cast<std::uint32_t>(c2) << 6) |
                                static_cast<std::uint32_t>(c3);

    out.push_back(static_cast<std::uint8_t>((block >> 16) & 0xff));
    if (compact[i + 2] != '=') {
      out.push_back(static_cast<std::uint8_t>((block >> 8) & 0xff));
    }
    if (compact[i + 3] != '=') {
      out.push_back(static_cast<std::uint8_t>(block & 0xff));
    }
  }

  return out;
}

int hex_value(char ch) {
  if (ch >= '0' && ch <= '9') {
    return ch - '0';
  }
  ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  if (ch >= 'A' && ch <= 'F') {
    return ch - 'A' + 10;
  }
  return -1;
}

std::map<std::string, std::string> parse_headers(const std::string& raw_headers) {
  std::map<std::string, std::string> headers;
  std::istringstream stream(raw_headers);
  std::string line;
  std::string current_key;

  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      continue;
    }
    if (!current_key.empty() && (line.front() == ' ' || line.front() == '\t')) {
      headers[current_key] += trim(line);
      continue;
    }

    const auto pos = line.find(':');
    if (pos == std::string::npos) {
      continue;
    }

    current_key = line.substr(0, pos);
    headers[current_key] = trim(line.substr(pos + 1));
  }

  return headers;
}

std::string get_boundary(const std::string& content_type) {
  const auto marker = content_type.find("boundary=");
  if (marker == std::string::npos) {
    return {};
  }

  auto value = content_type.substr(marker + 9);
  value = trim(value);
  if (!value.empty() && value.front() == '"') {
    const auto end = value.find('"', 1);
    if (end == std::string::npos) {
      throw std::runtime_error("unterminated MIME boundary");
    }
    return value.substr(1, end - 1);
  }

  const auto semicolon = value.find(';');
  return trim(value.substr(0, semicolon));
}

}  // namespace

std::string encode_quoted_printable(const std::vector<std::uint8_t>& bytes) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(bytes.size() * 3);
  for (const auto byte : bytes) {
    if ((byte >= 33 && byte <= 60) || (byte >= 62 && byte <= 126) || byte == ' ' || byte == '\t') {
      out.push_back(static_cast<char>(byte));
      continue;
    }
    if (byte == '\r') {
      out += "\r";
      continue;
    }
    if (byte == '\n') {
      out += "\n";
      continue;
    }
    out.push_back('=');
    out.push_back(kHex[(byte >> 4) & 0x0F]);
    out.push_back(kHex[byte & 0x0F]);
  }
  return out;
}

std::vector<std::uint8_t> decode_quoted_printable(const std::string& text) {
  std::vector<std::uint8_t> out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    const auto ch = text[i];
    if (ch != '=') {
      out.push_back(static_cast<std::uint8_t>(ch));
      continue;
    }
    if (i + 1 < text.size() && text[i + 1] == '\n') {
      ++i;
      continue;
    }
    if (i + 2 < text.size() && text[i + 1] == '\r' && text[i + 2] == '\n') {
      i += 2;
      continue;
    }
    if (i + 2 < text.size()) {
      const auto hi = hex_value(text[i + 1]);
      const auto lo = hex_value(text[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    out.push_back(static_cast<std::uint8_t>(ch));
  }
  return out;
}

std::string MimeMessage::render_multipart_mixed(const std::string& boundary) const {
  std::ostringstream out;
  for (const auto& [key, value] : headers) {
    out << key << ": " << value << "\r\n";
  }
  out << "MIME-Version: 1.0\r\n";
  out << "Content-Type: multipart/mixed; boundary=\"" << boundary << "\"\r\n\r\n";

  for (const auto& part : parts) {
    out << "--" << boundary << "\r\n";
    for (const auto& [key, value] : part.headers) {
      out << key << ": " << value << "\r\n";
    }

    const auto encoding_it = part.headers.find("Content-Transfer-Encoding");
    const bool use_base64 = encoding_it != part.headers.end() && encoding_it->second == "base64";
    const bool use_qp = encoding_it != part.headers.end() && encoding_it->second == "quoted-printable";
    out << "\r\n";

    if (use_base64) {
      out << base64_encode(part.body);
    } else if (use_qp) {
      out << encode_quoted_printable(part.body);
    } else {
      out.write(reinterpret_cast<const char*>(part.body.data()), static_cast<std::streamsize>(part.body.size()));
    }
    out << "\r\n";
  }

  out << "--" << boundary << "--\r\n";
  return out.str();
}

MimeMessage MimeMessage::parse(const std::string& raw_message) {
  const auto header_end = raw_message.find("\r\n\r\n");
  if (header_end == std::string::npos) {
    throw std::runtime_error("invalid MIME message: missing header separator");
  }

  MimeMessage message;
  message.headers = parse_headers(raw_message.substr(0, header_end));

  const auto ct_it = message.headers.find("Content-Type");
  if (ct_it == message.headers.end()) {
    throw std::runtime_error("invalid MIME message: missing content type");
  }

  const auto boundary = get_boundary(ct_it->second);
  if (boundary.empty()) {
    throw std::runtime_error("invalid MIME message: missing multipart boundary");
  }

  const std::string marker = "--" + boundary;
  std::size_t cursor = header_end + 4;

  while (true) {
    const auto part_start = raw_message.find(marker, cursor);
    if (part_start == std::string::npos) {
      break;
    }

    cursor = part_start + marker.size();
    if (cursor + 1 < raw_message.size() && raw_message[cursor] == '-' && raw_message[cursor + 1] == '-') {
      break;
    }

    if (raw_message.compare(cursor, 2, "\r\n") == 0) {
      cursor += 2;
    }

    const auto part_header_end = raw_message.find("\r\n\r\n", cursor);
    if (part_header_end == std::string::npos) {
      throw std::runtime_error("invalid MIME part: missing header separator");
    }

    MimePart part;
    part.headers = parse_headers(raw_message.substr(cursor, part_header_end - cursor));
    cursor = part_header_end + 4;

    const auto next_marker = raw_message.find("\r\n" + marker, cursor);
    if (next_marker == std::string::npos) {
      throw std::runtime_error("invalid MIME message: unterminated boundary");
    }

    auto body_text = raw_message.substr(cursor, next_marker - cursor);
    const auto encoding_it = part.headers.find("Content-Transfer-Encoding");
    if (encoding_it != part.headers.end() && encoding_it->second == "base64") {
      part.body = base64_decode(body_text);
    } else if (encoding_it != part.headers.end() && encoding_it->second == "quoted-printable") {
      part.body = decode_quoted_printable(body_text);
    } else {
      part.body.assign(body_text.begin(), body_text.end());
    }

    message.parts.push_back(std::move(part));
    cursor = next_marker + 2;
  }

  return message;
}

std::string make_boundary() {
  const auto now = static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  std::mt19937_64 rng(now);
  std::uniform_int_distribution<std::uint64_t> dist;

  std::ostringstream out;
  out << "mailfs-boundary-" << std::hex << dist(rng) << dist(rng);
  return out.str();
}

}  // namespace mailfs::core::mime
