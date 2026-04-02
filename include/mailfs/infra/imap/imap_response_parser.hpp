#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mailfs::infra::imap {

struct TaggedStatus {
  std::string tag;
  std::string status;
  std::string text;
};

struct ListMailboxName {
  std::string raw_name;
  std::string decoded_name;
};

class ImapResponseParser {
 public:
  static std::optional<std::size_t> literal_size_from_line(std::string_view line);
  static std::optional<TaggedStatus> parse_tagged_status(std::string_view line, std::string_view tag);
  static std::optional<std::string> parse_list_mailbox(std::string_view line,
                                                       const std::string& literal_mailbox = {});
  static std::optional<ListMailboxName> parse_list_mailbox_name(std::string_view line,
                                                                const std::string& literal_mailbox = {});
  static std::vector<std::uint64_t> parse_search_uids(std::string_view line);
  static std::optional<std::uint64_t> parse_fetch_uid(std::string_view line);
  static std::optional<std::uint64_t> parse_append_uid(std::string_view text);
};

}  // namespace mailfs::infra::imap
