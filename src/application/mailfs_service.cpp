#include "mailfs/application/mailfs_service.hpp"

#include <chrono>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>

#include "mailfs/core/hash.hpp"
#include "mailfs/core/mime/mime_message.hpp"
#include "mailfs/core/model/mail_block_metadata.hpp"
#include "mailfs/core/security/xor_codec.hpp"

namespace mailfs::application {

namespace {

std::string trim_crlf(std::string text) {
  while (!text.empty() && (text.back() == '\r' || text.back() == '\n')) {
    text.pop_back();
  }
  return text;
}

std::string now_iso8601_utc() {
  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::tm utc_tm{};
#ifdef _WIN32
  gmtime_s(&utc_tm, &time);
#else
  gmtime_r(&time, &utc_tm);
#endif
  std::ostringstream out;
  out << std::put_time(&utc_tm, "%Y-%m-%dT%H:%M:%SZ");
  return out.str();
}

std::string now_rfc2822_utc() {
  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::tm utc_tm{};
#ifdef _WIN32
  gmtime_s(&utc_tm, &time);
#else
  gmtime_r(&time, &utc_tm);
#endif
  std::ostringstream out;
  out << std::put_time(&utc_tm, "%a, %d %b %Y %H:%M:%S +0000");
  return out.str();
}

std::vector<unsigned char> read_block(std::ifstream& input, std::size_t max_bytes) {
  std::vector<unsigned char> data(max_bytes);
  input.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(max_bytes));
  data.resize(static_cast<std::size_t>(input.gcount()));
  return data;
}

core::model::MailBlockMetadata extract_metadata_from_message(const std::string& raw_message) {
  const auto message = core::mime::MimeMessage::parse(raw_message);
  for (const auto& part : message.parts) {
    const auto it = part.headers.find("Content-Type");
    if (it == part.headers.end()) {
      continue;
    }
    if (it->second.find("application/json") != std::string::npos || it->second.find("text/plain") != std::string::npos) {
      const std::string json_text(part.body.begin(), part.body.end());
      auto metadata = core::model::MailBlockMetadata::from_json_text(json_text);
      if (metadata.encrypted) {
        metadata.local_path = core::security::decrypt_string(metadata.local_path);
      }
      return metadata;
    }
  }
  throw std::runtime_error("mailfs metadata part not found");
}

std::vector<unsigned char> extract_attachment_from_message(const std::string& raw_message) {
  const auto message = core::mime::MimeMessage::parse(raw_message);
  for (const auto& part : message.parts) {
    const auto it = part.headers.find("Content-Disposition");
    if (it != part.headers.end() && it->second.find("attachment") != std::string::npos) {
      return part.body;
    }
  }
  throw std::runtime_error("mailfs attachment part not found");
}

std::string build_message(const core::model::MailBlockMetadata& metadata,
                          const std::string& username,
                          const std::string& email_name,
                          const std::vector<unsigned char>& payload) {
  core::mime::MimeMessage message;
  message.headers["From"] = "\"" + email_name + "\" <" + username + ">";
  message.headers["To"] = "\"" + email_name + "\" <" + username + ">";
  message.headers["Subject"] = metadata.subject;
  message.headers["Date"] = now_rfc2822_utc();

  core::mime::MimePart metadata_part;
  metadata_part.headers["Content-Type"] = "application/json; charset=utf-8";
  metadata_part.headers["Content-Transfer-Encoding"] = "base64";
  const auto metadata_text = metadata.to_json_text();
  metadata_part.body.assign(metadata_text.begin(), metadata_text.end());

  core::mime::MimePart file_part;
  file_part.headers["Content-Type"] = "application/octet-stream";
  file_part.headers["Content-Transfer-Encoding"] = "base64";
  file_part.headers["Content-Disposition"] =
      "attachment; filename=\"block-" + std::to_string(metadata.block_seq) + ".bin\"";
  file_part.body = payload;

  message.parts.push_back(std::move(metadata_part));
  message.parts.push_back(std::move(file_part));
  return message.render_multipart_mixed(core::mime::make_boundary());
}

}  // namespace

MailfsService::MailfsService(core::model::AppConfig config,
                             ports::IMailTransport& transport,
                             ports::ICacheRepository& repository)
    : config_(std::move(config)), transport_(transport), repository_(repository) {
  repository_.initialize();
}

void MailfsService::connect() {
  if (connected_) {
    return;
  }

  auto credentials = load_credentials();
  username_ = std::move(credentials.first);
  password_ = std::move(credentials.second);
  transport_.connect(config_, username_, password_);
  connected_ = true;
}

void MailfsService::disconnect() noexcept {
  transport_.disconnect();
  connected_ = false;
}

std::vector<std::string> MailfsService::list_mailboxes() {
  ensure_connected();
  auto mailboxes = transport_.list_mailboxes(config_.mailbox_prefix.empty() ? "*" : config_.mailbox_prefix);

  std::vector<std::string> filtered;
  for (const auto& mailbox : mailboxes) {
    if (config_.is_allowed_mailbox(mailbox)) {
      filtered.push_back(mailbox);
    }
  }
  return filtered;
}

std::size_t MailfsService::cache_mailbox(const std::string& mailbox) {
  ensure_mailbox_selected(mailbox);

  const auto all_uids = transport_.search_all_uids();
  const auto cached_uids = repository_.get_cached_uids(mailbox);

  std::vector<std::uint64_t> uncached_uids;
  for (const auto uid : all_uids) {
    if (cached_uids.find(uid) == cached_uids.end()) {
      uncached_uids.push_back(uid);
    }
  }

  if (uncached_uids.empty()) {
    return 0;
  }

  const auto messages = transport_.fetch_messages(uncached_uids);
  for (const auto& message : messages) {
    auto metadata = extract_metadata_from_message(message.raw_message);
    if (metadata.mail_folder.empty()) {
      metadata.mail_folder = mailbox;
    }
    repository_.upsert_mail_block(message.uid, metadata);
  }

  return messages.size();
}

std::vector<core::model::CachedFileRecord> MailfsService::list_cached_files(const std::string& mailbox) {
  return repository_.list_files(mailbox);
}

void MailfsService::upload_file(const std::string& mailbox,
                                const std::filesystem::path& local_file,
                                const std::string& remote_path) {
  ensure_mailbox_selected(mailbox);

  if (!std::filesystem::exists(local_file)) {
    throw std::runtime_error("local file not found: " + local_file.string());
  }

  const auto file_size = std::filesystem::file_size(local_file);
  const auto block_size = config_.block_size_for_file(local_file);
  const auto block_count =
      static_cast<std::int32_t>((file_size + block_size - 1) / block_size);
  const auto file_md5 = core::md5_hex(local_file);
  const auto encrypted = core::security::should_encrypt_mailbox(mailbox);
  const auto subject_name = encrypted
                                ? core::security::encrypt_string(std::filesystem::path(remote_path).filename().string())
                                : std::filesystem::path(remote_path).filename().string();
  const auto stored_remote_path = encrypted ? core::security::encrypt_string(remote_path) : remote_path;

  std::ifstream input(local_file, std::ios::binary);
  if (!input) {
    throw std::runtime_error("failed to open file for upload: " + local_file.string());
  }

  for (std::int32_t block_seq = 1; block_seq <= block_count; ++block_seq) {
    auto payload = read_block(input, block_size);

    core::model::MailBlockMetadata metadata;
    metadata.subject = core::model::MailBlockMetadata::make_subject(subject_name, encrypted, block_seq, block_count);
    metadata.file_md5 = file_md5;
    metadata.block_md5 = core::md5_hex(payload);
    metadata.file_size = file_size;
    metadata.block_size = payload.size();
    metadata.create_time = now_iso8601_utc();
    metadata.owner = username_;
    metadata.local_path = stored_remote_path;
    metadata.mail_folder = mailbox;
    metadata.block_seq = block_seq;
    metadata.block_count = block_count;
    metadata.encrypted = encrypted;

    transport_.append_message(mailbox, build_message(metadata, username_, config_.email_name, payload));
  }
}

void MailfsService::download_file(const std::string& mailbox,
                                  const std::string& remote_path,
                                  const std::filesystem::path& output_file) {
  ensure_mailbox_selected(mailbox);

  auto cached = repository_.find_file(mailbox, remote_path);
  if (!cached.has_value()) {
    cache_mailbox(mailbox);
    cached = repository_.find_file(mailbox, remote_path);
  }

  if (!cached.has_value()) {
    throw std::runtime_error("cached file index not found for remote path: " + remote_path);
  }

  auto file_record = *cached;
  file_record.sort_blocks();

  std::vector<std::uint64_t> uids;
  for (const auto& block : file_record.blocks) {
    uids.push_back(block.uid);
  }

  const auto fetched_messages = transport_.fetch_messages(uids);
  std::map<std::uint64_t, std::string> by_uid;
  for (const auto& message : fetched_messages) {
    by_uid.emplace(message.uid, message.raw_message);
  }

  if (!output_file.parent_path().empty()) {
    std::filesystem::create_directories(output_file.parent_path());
  }
  std::ofstream output(output_file, std::ios::binary);
  if (!output) {
    throw std::runtime_error("failed to open output file: " + output_file.string());
  }

  for (const auto& block : file_record.blocks) {
    const auto it = by_uid.find(block.uid);
    if (it == by_uid.end()) {
      throw std::runtime_error("missing downloaded block uid: " + std::to_string(block.uid));
    }

    const auto payload = extract_attachment_from_message(it->second);
    output.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
  }
}

void MailfsService::ensure_connected() {
  if (!connected_) {
    connect();
  }
}

void MailfsService::ensure_mailbox_selected(const std::string& mailbox) {
  ensure_connected();
  transport_.select_mailbox(mailbox);
}

std::pair<std::string, std::string> MailfsService::load_credentials() const {
  std::ifstream input(config_.credential_file);
  if (!input) {
    throw std::runtime_error("failed to open credential file: " + config_.credential_file);
  }

  std::string username;
  std::string password;
  std::getline(input, username);
  std::getline(input, password);

  username = trim_crlf(username);
  password = trim_crlf(password);

  if (username.empty() || password.empty()) {
    throw std::runtime_error("credential file must contain username and password");
  }

  return {username, password};
}

}  // namespace mailfs::application
