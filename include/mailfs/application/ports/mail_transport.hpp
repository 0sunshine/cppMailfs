#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "mailfs/core/model/app_config.hpp"

namespace mailfs::application::ports {

struct FetchedMessage {
  std::uint64_t uid = 0;
  std::string raw_message;
};

struct FetchedMetadata {
  std::uint64_t uid = 0;
  std::string metadata_text;
};

class IMailTransport {
 public:
  virtual ~IMailTransport() = default;

  virtual void connect(const core::model::AppConfig& config,
                       const std::string& username,
                       const std::string& password) = 0;
  virtual void disconnect() noexcept = 0;
  virtual std::vector<std::string> list_mailboxes(const std::string& pattern) = 0;
  virtual void select_mailbox(const std::string& mailbox) = 0;
  virtual std::vector<std::uint64_t> search_all_uids() = 0;
  virtual std::vector<FetchedMetadata> fetch_metadata(const std::vector<std::uint64_t>& uids) = 0;
  virtual std::vector<FetchedMessage> fetch_messages(const std::vector<std::uint64_t>& uids) = 0;
  virtual void delete_message_by_uid(std::uint64_t uid) = 0;
  virtual std::optional<std::uint64_t> append_message(const std::string& mailbox,
                                                      const std::string& raw_message) = 0;
};

}  // namespace mailfs::application::ports
