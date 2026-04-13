#include "mailfs/infra/imap/imap_client.hpp"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <thread>

#include "mailfs/infra/logging/logger.hpp"
#include "mailfs/infra/imap/imap_utf7.hpp"

namespace mailfs::infra::imap {

namespace {

std::string join_uids(const std::vector<std::uint64_t>& uids) {
  std::ostringstream stream;
  for (std::size_t i = 0; i < uids.size(); ++i) {
    if (i > 0) {
      stream << ',';
    }
    stream << uids[i];
  }
  return stream.str();
}

std::string sanitize_command_for_log(std::string_view command) {
  if (command.rfind("LOGIN ", 0) == 0) {
    return "LOGIN <redacted>";
  }
  return std::string(command);
}

bool is_retryable_system_busy(const CommandResponse& response) {
  return response.status == "NO" && response.text.find("System busy") != std::string::npos;
}

bool starts_with(std::string_view text, std::string_view prefix) {
  return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

bool contains(std::string_view text, std::string_view needle) {
  return text.find(needle) != std::string_view::npos;
}

bool is_retryable_network_failure(const std::runtime_error& ex) {
  const auto text = std::string_view(ex.what());
  return text == "TLS read timed out while waiting for server response" ||
         text == "remote side closed the TLS connection" ||
         text == "socket is not connected" ||
         starts_with(text, "mbedtls_net_connect failed:") ||
         starts_with(text, "mbedtls_net_") ||
         starts_with(text, "mbedtls_ssl_handshake failed:") ||
         starts_with(text, "mbedtls_ssl_read failed:") ||
         starts_with(text, "mbedtls_ssl_write failed:") ||
         contains(text, "Failed to get an IP address for the given hostname");
}

}  // namespace

ImapClient::~ImapClient() {
  disconnect();
}

void ImapClient::connect(const core::model::AppConfig& config,
                         const std::string& username,
                         const std::string& password) {
  connection_config_ = config;
  username_ = username;
  password_ = password;
  selected_mailbox_.clear();

  constexpr auto kRetryDelay = std::chrono::seconds(3);
  constexpr int kMaxRetries = 3;

  for (int attempt = 0;; ++attempt) {
    try {
      open_authenticated_session();
      return;
    } catch (const std::runtime_error& ex) {
      socket_.close();
      connected_ = false;
      tag_counter_ = 0;
      if (!is_retryable_network_failure(ex) || attempt >= kMaxRetries) {
        throw;
      }
      logging::log_warn(
          "imap",
          "IMAP connect hit a retryable network failure; reconnecting in 3 seconds (" +
              std::to_string(attempt + 1) + "/" + std::to_string(kMaxRetries) + "): " + ex.what());
      std::this_thread::sleep_for(kRetryDelay);
    }
  }
}

void ImapClient::disconnect() noexcept {
  const auto was_connected = connected_;
  if (socket_.is_open()) {
    try {
      execute("LOGOUT");
    } catch (...) {
    }
  }
  socket_.close();
  connected_ = false;
  selected_mailbox_.clear();
  if (was_connected) {
    logging::log_info("imap", "disconnected");
  }
}

std::vector<std::string> ImapClient::list_mailboxes(const std::string& pattern) {
  logging::log_debug("imap", "LIST mailbox pattern " + pattern);
  const auto response = execute("LIST \"\" " + quote_string(encode_imap_utf7(pattern)));
  ensure_ok(response, "LIST");

  std::vector<std::string> mailboxes;
  for (const auto& chunk : response.chunks) {
    if (const auto mailbox = ImapResponseParser::parse_list_mailbox(chunk.line, chunk.literal)) {
      mailboxes.push_back(*mailbox);
    }
  }
  return mailboxes;
}

void ImapClient::select_mailbox(const std::string& mailbox) {
  select_mailbox_once(mailbox);
  selected_mailbox_ = mailbox;
}

std::vector<std::uint64_t> ImapClient::search_all_uids() {
  logging::log_debug("imap", "UID SEARCH ALL");
  const auto response = execute("UID SEARCH ALL");
  ensure_ok(response, "UID SEARCH");

  std::vector<std::uint64_t> uids;
  for (const auto& chunk : response.chunks) {
    auto parsed = ImapResponseParser::parse_search_uids(chunk.line);
    uids.insert(uids.end(), parsed.begin(), parsed.end());
  }
  return uids;
}

std::vector<application::ports::FetchedMessage> ImapClient::fetch_messages(const std::vector<std::uint64_t>& uids) {
  if (uids.empty()) {
    return {};
  }

  logging::log_debug("imap", "UID FETCH message_count=" + std::to_string(uids.size()));

  const auto response = execute("UID FETCH " + join_uids(uids) + " (UID BODY.PEEK[])");
  ensure_ok(response, "UID FETCH");

  std::vector<application::ports::FetchedMessage> messages;
  for (const auto& chunk : response.chunks) {
    if (chunk.literal.empty()) {
      continue;
    }
    const auto uid = ImapResponseParser::parse_fetch_uid(chunk.line);
    if (!uid) {
      continue;
    }
    messages.push_back({*uid, chunk.literal});
  }

  std::sort(messages.begin(), messages.end(), [](const auto& lhs, const auto& rhs) {
    return lhs.uid < rhs.uid;
  });
  return messages;
}

std::vector<application::ports::FetchedMetadata> ImapClient::fetch_metadata(const std::vector<std::uint64_t>& uids) {
  if (uids.empty()) {
    return {};
  }

  logging::log_debug("imap", "UID FETCH metadata_count=" + std::to_string(uids.size()));

  const auto response = execute("UID FETCH " + join_uids(uids) + " (UID BODY.PEEK[1])");
  ensure_ok(response, "UID FETCH");

  std::vector<application::ports::FetchedMetadata> metadata_entries;
  for (const auto& chunk : response.chunks) {
    if (chunk.literal.empty()) {
      continue;
    }
    const auto uid = ImapResponseParser::parse_fetch_uid(chunk.line);
    if (!uid) {
      continue;
    }
    metadata_entries.push_back({*uid, chunk.literal});
  }

  std::sort(metadata_entries.begin(), metadata_entries.end(), [](const auto& lhs, const auto& rhs) {
    return lhs.uid < rhs.uid;
  });
  return metadata_entries;
}

void ImapClient::delete_message_by_uid(std::uint64_t uid) {
  logging::log_warn("imap", "UID STORE + EXPUNGE uid=" + std::to_string(uid));

  constexpr auto kRetryDelay = std::chrono::seconds(3);
  constexpr int kMaxBusyRetries = 3;

  for (int attempt = 0;; ++attempt) {
    const auto store_response = execute("UID STORE " + std::to_string(uid) + " +FLAGS.SILENT (\\Deleted)");
    if (!is_retryable_system_busy(store_response) || attempt >= kMaxBusyRetries) {
      ensure_ok(store_response, "UID STORE");
      break;
    }

    logging::log_warn("imap",
                      "UID STORE returned NO System busy; retrying in 3 seconds (" + std::to_string(attempt + 1) +
                          "/" + std::to_string(kMaxBusyRetries) + ")");
    std::this_thread::sleep_for(kRetryDelay);
  }

  for (int attempt = 0;; ++attempt) {
    const auto expunge_response = execute("EXPUNGE");
    if (!is_retryable_system_busy(expunge_response) || attempt >= kMaxBusyRetries) {
      ensure_ok(expunge_response, "EXPUNGE");
      break;
    }

    logging::log_warn("imap",
                      "EXPUNGE returned NO System busy; retrying in 3 seconds (" + std::to_string(attempt + 1) +
                          "/" + std::to_string(kMaxBusyRetries) + ")");
    std::this_thread::sleep_for(kRetryDelay);
  }
}

std::optional<std::uint64_t> ImapClient::append_message(const std::string& mailbox, const std::string& raw_message) {
  std::string payload = raw_message;
  if (payload.size() < 2 || payload.substr(payload.size() - 2) != "\r\n") {
    payload += "\r\n";
  }

  constexpr auto kRetryDelay = std::chrono::seconds(3);
  constexpr int kMaxBusyRetries = 3;
  const auto command = "APPEND " + quote_string(encode_imap_utf7(mailbox)) + " {" + std::to_string(payload.size()) + "}";

  for (int attempt = 0;; ++attempt) {
    logging::log_debug("imap", "APPEND mailbox " + mailbox + " bytes=" + std::to_string(payload.size()));
    const auto response = execute(command, [&] {
      socket_.send_all(payload);
    });
    if (!is_retryable_system_busy(response) || attempt >= kMaxBusyRetries) {
      ensure_ok(response, "APPEND");
      return ImapResponseParser::parse_append_uid(response.text);
    }

    logging::log_warn("imap",
                      "APPEND returned NO System busy; retrying in 3 seconds (" + std::to_string(attempt + 1) + "/" +
                          std::to_string(kMaxBusyRetries) + ")");
    std::this_thread::sleep_for(kRetryDelay);
  }
}

void ImapClient::open_authenticated_session() {
  if (!connection_config_.has_value()) {
    throw std::runtime_error("IMAP connection configuration is not initialized");
  }

  logging::log_info("imap",
                    "connecting to " + connection_config_->imap_host + ":" + std::to_string(connection_config_->imap_port));
  socket_.connect(connection_config_->imap_host,
                  connection_config_->imap_port,
                  connection_config_->allow_insecure_tls,
                  std::filesystem::u8path(connection_config_->ca_cert_file));
  tag_counter_ = 0;

  const auto greeting = socket_.read_line();
  if (greeting.rfind("* OK", 0) != 0 && greeting.rfind("* PREAUTH", 0) != 0) {
    throw std::runtime_error("IMAP greeting rejected: " + greeting);
  }

  ensure_ok(execute_once("LOGIN " + quote_string(username_) + " " + quote_string(password_)), "LOGIN");
  connected_ = true;
  logging::log_info("imap", "LOGIN completed");
}

void ImapClient::reconnect_after_connection_failure() {
  if (!connection_config_.has_value() || username_.empty()) {
    throw std::runtime_error("cannot reconnect IMAP session because credentials are unavailable");
  }

  const auto mailbox = selected_mailbox_;
  socket_.close();
  connected_ = false;
  tag_counter_ = 0;
  open_authenticated_session();
  if (!mailbox.empty()) {
    select_mailbox_once(mailbox);
    selected_mailbox_ = mailbox;
  }
}

void ImapClient::select_mailbox_once(const std::string& mailbox) {
  std::string raw_mailbox = encode_imap_utf7(mailbox);
  const auto list_response = execute("LIST \"\" " + quote_string(raw_mailbox));
  ensure_ok(list_response, "LIST");
  for (const auto& chunk : list_response.chunks) {
    const auto parsed = ImapResponseParser::parse_list_mailbox_name(chunk.line, chunk.literal);
    if (parsed.has_value() && parsed->decoded_name == mailbox) {
      raw_mailbox = parsed->raw_name;
      break;
    }
  }
  logging::log_debug("imap", "SELECT mailbox " + mailbox + " raw=" + raw_mailbox);
  ensure_ok(execute("SELECT " + quote_string(raw_mailbox)), "SELECT");
}

std::string ImapClient::next_tag() {
  std::ostringstream stream;
  stream << active_tag_prefix_ << std::setw(4) << std::setfill('0') << ++tag_counter_;
  return stream.str();
}

CommandResponse ImapClient::execute(const std::string& command, const std::function<void()>& continuation_writer) {
  constexpr auto kRetryDelay = std::chrono::seconds(3);
  constexpr int kMaxRetries = 3;

  for (int attempt = 0;; ++attempt) {
    try {
      return execute_once(command, continuation_writer);
    } catch (const std::runtime_error& ex) {
      if (!is_retryable_network_failure(ex) || attempt >= kMaxRetries) {
        throw;
      }
      logging::log_warn("imap",
                        "command hit a retryable network failure; reconnecting in 3 seconds (" +
                            std::to_string(attempt + 1) + "/" + std::to_string(kMaxRetries) + "): " + ex.what());
      std::this_thread::sleep_for(kRetryDelay);
      reconnect_after_connection_failure();
    }
  }
}

CommandResponse ImapClient::execute_once(const std::string& command, const std::function<void()>& continuation_writer) {
  const auto tag = next_tag();
  if (logging::Logger::instance().should_log(logging::LogLevel::kDebug)) {
    logging::log_debug("imap", "sending command " + tag + " " + sanitize_command_for_log(command));
  }
  socket_.send_all(tag + " " + command + "\r\n");

  CommandResponse response;
  response.tag = tag;
  bool continuation_sent = false;

  while (true) {
    const auto line = socket_.read_line();

    if (!continuation_sent && !line.empty() && line.front() == '+' && continuation_writer) {
      continuation_writer();
      continuation_sent = true;
      continue;
    }

    ResponseChunk chunk;
    chunk.line = line;
    if (const auto literal_size = ImapResponseParser::literal_size_from_line(line)) {
      chunk.literal = socket_.read_exact(*literal_size);
    }

    if (const auto status = ImapResponseParser::parse_tagged_status(line, tag)) {
      response.status = status->status;
      response.text = status->text;
      logging::log_debug("imap", "received status " + tag + " " + response.status + " " + response.text);
      break;
    }

    response.chunks.push_back(std::move(chunk));
  }

  return response;
}

std::string ImapClient::quote_string(const std::string& value) {
  std::string escaped;
  escaped.reserve(value.size() + 2);
  escaped.push_back('"');
  for (const auto ch : value) {
    if (ch == '"' || ch == '\\') {
      escaped.push_back('\\');
    }
    escaped.push_back(ch);
  }
  escaped.push_back('"');
  return escaped;
}

void ImapClient::ensure_ok(const CommandResponse& response, const std::string& command_name) {
  if (response.status != "OK") {
    throw std::runtime_error(command_name + " failed: " + response.status + " " + response.text);
  }
}

}  // namespace mailfs::infra::imap
