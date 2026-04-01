#include "mailfs/infra/imap/imap_response_parser.hpp"

#include <regex>
#include <sstream>

namespace mailfs::infra::imap {

std::optional<std::size_t> ImapResponseParser::literal_size_from_line(std::string_view line) {
  static const std::regex pattern(R"(\{(\d+)\}\s*$)");
  std::match_results<std::string_view::const_iterator> match;
  if (!std::regex_search(line.begin(), line.end(), match, pattern)) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(std::stoull(match[1].str()));
}

std::optional<TaggedStatus> ImapResponseParser::parse_tagged_status(std::string_view line, std::string_view tag) {
  if (line.substr(0, tag.size()) != tag) {
    return std::nullopt;
  }

  std::istringstream stream{std::string(line)};
  TaggedStatus status;
  stream >> status.tag >> status.status;
  std::getline(stream, status.text);
  if (!status.text.empty() && status.text.front() == ' ') {
    status.text.erase(status.text.begin());
  }
  return status;
}

std::optional<std::string> ImapResponseParser::parse_list_mailbox(std::string_view line,
                                                                  const std::string& literal_mailbox) {
  if (line.rfind("* LIST", 0) != 0) {
    return std::nullopt;
  }

  if (!literal_mailbox.empty()) {
    return literal_mailbox;
  }

  if (line.size() >= 2 && line.back() == '"' && line.find('"') != std::string_view::npos) {
    const auto last_quote = line.find_last_of('"');
    const auto prev_quote = line.substr(0, last_quote).find_last_of('"');
    if (prev_quote != std::string_view::npos) {
      return std::string(line.substr(prev_quote + 1, last_quote - prev_quote - 1));
    }
  }

  const auto last_space = line.find_last_of(' ');
  if (last_space == std::string_view::npos || last_space + 1 >= line.size()) {
    return std::nullopt;
  }

  return std::string(line.substr(last_space + 1));
}

std::vector<std::uint64_t> ImapResponseParser::parse_search_uids(std::string_view line) {
  std::vector<std::uint64_t> uids;
  if (line.rfind("* SEARCH", 0) != 0) {
    return uids;
  }

  std::istringstream stream{std::string(line.substr(8))};
  std::uint64_t uid = 0;
  while (stream >> uid) {
    uids.push_back(uid);
  }
  return uids;
}

std::optional<std::uint64_t> ImapResponseParser::parse_fetch_uid(std::string_view line) {
  static const std::regex pattern(R"(UID\s+(\d+))");
  std::match_results<std::string_view::const_iterator> match;
  if (!std::regex_search(line.begin(), line.end(), match, pattern)) {
    return std::nullopt;
  }
  return std::stoull(match[1].str());
}

}  // namespace mailfs::infra::imap
