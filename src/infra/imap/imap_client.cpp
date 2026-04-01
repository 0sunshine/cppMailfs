#include "mailfs/infra/imap/imap_client.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <stdexcept>

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

}  // namespace

ImapClient::~ImapClient() {
  disconnect();
}

void ImapClient::connect(const core::model::AppConfig& config,
                         const std::string& username,
                         const std::string& password) {
  socket_.connect(config.imap_host, config.imap_port, config.allow_insecure_tls);

  const auto greeting = socket_.read_line();
  if (greeting.rfind("* OK", 0) != 0 && greeting.rfind("* PREAUTH", 0) != 0) {
    throw std::runtime_error("IMAP greeting rejected: " + greeting);
  }

  ensure_ok(execute("LOGIN " + quote_string(username) + " " + quote_string(password)), "LOGIN");
  connected_ = true;
}

void ImapClient::disconnect() noexcept {
  if (socket_.is_open()) {
    try {
      execute("LOGOUT");
    } catch (...) {
    }
  }
  socket_.close();
  connected_ = false;
}

std::vector<std::string> ImapClient::list_mailboxes(const std::string& pattern) {
  const auto response = execute("LIST \"\" " + quote_string(pattern));
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
  ensure_ok(execute("SELECT " + quote_string(mailbox)), "SELECT");
}

std::vector<std::uint64_t> ImapClient::search_all_uids() {
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

void ImapClient::delete_message_by_uid(std::uint64_t uid) {
  ensure_ok(execute("UID STORE " + std::to_string(uid) + " +FLAGS.SILENT (\\Deleted)"), "UID STORE");
  ensure_ok(execute("EXPUNGE"), "EXPUNGE");
}

void ImapClient::append_message(const std::string& mailbox, const std::string& raw_message) {
  std::string payload = raw_message;
  if (payload.size() < 2 || payload.substr(payload.size() - 2) != "\r\n") {
    payload += "\r\n";
  }

  const auto response = execute("APPEND " + quote_string(mailbox) + " {" + std::to_string(payload.size()) + "}", [&] {
    socket_.send_all(payload);
  });
  ensure_ok(response, "APPEND");
}

std::string ImapClient::next_tag() {
  std::ostringstream stream;
  stream << active_tag_prefix_ << std::setw(4) << std::setfill('0') << ++tag_counter_;
  return stream.str();
}

CommandResponse ImapClient::execute(const std::string& command, const std::function<void()>& continuation_writer) {
  const auto tag = next_tag();
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
