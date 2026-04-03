#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "mailfs/application/ports/mail_transport.hpp"
#include "mailfs/infra/imap/imap_response_parser.hpp"
#include "mailfs/infra/net/secure_socket.hpp"

namespace mailfs::infra::imap {

struct ResponseChunk {
  std::string line;
  std::string literal;
};

struct CommandResponse {
  std::string tag;
  std::string status;
  std::string text;
  std::vector<ResponseChunk> chunks;
};

class ImapClient final : public application::ports::IMailTransport {
 public:
  ImapClient() = default;
  ~ImapClient() override;

  void connect(const core::model::AppConfig& config,
               const std::string& username,
               const std::string& password) override;
  void disconnect() noexcept override;
  std::vector<std::string> list_mailboxes(const std::string& pattern) override;
  void select_mailbox(const std::string& mailbox) override;
  std::vector<std::uint64_t> search_all_uids() override;
  std::vector<application::ports::FetchedMetadata> fetch_metadata(const std::vector<std::uint64_t>& uids) override;
  std::vector<application::ports::FetchedMessage> fetch_messages(const std::vector<std::uint64_t>& uids) override;
  void delete_message_by_uid(std::uint64_t uid) override;
  std::optional<std::uint64_t> append_message(const std::string& mailbox, const std::string& raw_message) override;

 private:
  net::SecureSocket socket_;
  std::optional<core::model::AppConfig> connection_config_;
  std::string username_;
  std::string password_;
  std::string selected_mailbox_;
  std::string active_tag_prefix_ = "A";
  std::size_t tag_counter_ = 0;
  bool connected_ = false;

  [[nodiscard]] std::string next_tag();
  void open_authenticated_session();
  void reconnect_after_timeout();
  void select_mailbox_once(const std::string& mailbox);
  CommandResponse execute(const std::string& command,
                          const std::function<void()>& continuation_writer = {});
  CommandResponse execute_once(const std::string& command,
                               const std::function<void()>& continuation_writer = {});
  static std::string quote_string(const std::string& value);
  static void ensure_ok(const CommandResponse& response, const std::string& command_name);
};

}  // namespace mailfs::infra::imap
