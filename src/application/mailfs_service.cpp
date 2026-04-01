#include "mailfs/application/mailfs_service.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>

#include "mailfs/core/hash.hpp"
#include "mailfs/core/mime/mime_message.hpp"
#include "mailfs/core/model/mail_block_metadata.hpp"
#include "mailfs/core/security/xor_codec.hpp"
#include "mailfs/infra/logging/logger.hpp"

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

std::string decode_quoted_printable(std::string_view text) {
  auto decode_hex = [](char ch) -> int {
    if (ch >= '0' && ch <= '9') {
      return ch - '0';
    }
    ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    if (ch >= 'A' && ch <= 'F') {
      return ch - 'A' + 10;
    }
    return -1;
  };

  std::string decoded;
  decoded.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '=') {
      decoded.push_back(text[i]);
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
      const auto high = decode_hex(text[i + 1]);
      const auto low = decode_hex(text[i + 2]);
      if (high >= 0 && low >= 0) {
        decoded.push_back(static_cast<char>((high << 4) | low));
        i += 2;
        continue;
      }
    }
    decoded.push_back(text[i]);
  }
  return decoded;
}

std::string decode_base64(std::string_view text) {
  auto decode_char = [](unsigned char ch) -> int {
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
    if (ch == '/') {
      return 63;
    }
    if (ch == '=') {
      return -2;
    }
    return -1;
  };

  std::string compact;
  compact.reserve(text.size());
  for (const auto ch : text) {
    if (!std::isspace(static_cast<unsigned char>(ch))) {
      compact.push_back(static_cast<char>(ch));
    }
  }

  if (compact.empty() || (compact.size() % 4) != 0) {
    return {};
  }

  std::string decoded;
  decoded.reserve((compact.size() / 4) * 3);
  for (std::size_t i = 0; i < compact.size(); i += 4) {
    const auto a = decode_char(static_cast<unsigned char>(compact[i]));
    const auto b = decode_char(static_cast<unsigned char>(compact[i + 1]));
    const auto c = decode_char(static_cast<unsigned char>(compact[i + 2]));
    const auto d = decode_char(static_cast<unsigned char>(compact[i + 3]));
    if (a < 0 || b < 0 || c == -1 || d == -1) {
      return {};
    }

    const auto triple = (static_cast<unsigned int>(a) << 18) | (static_cast<unsigned int>(b) << 12) |
                        (static_cast<unsigned int>(c < 0 ? 0 : c) << 6) | static_cast<unsigned int>(d < 0 ? 0 : d);
    decoded.push_back(static_cast<char>((triple >> 16) & 0xFF));
    if (c != -2) {
      decoded.push_back(static_cast<char>((triple >> 8) & 0xFF));
    }
    if (d != -2) {
      decoded.push_back(static_cast<char>(triple & 0xFF));
    }
  }

  return decoded;
}

core::model::MailBlockMetadata parse_cached_metadata_text(std::string_view raw_text) {
  const auto is_meaningful = [](const core::model::MailBlockMetadata& metadata) {
    return !metadata.subject.empty() || !metadata.local_path.empty() || metadata.block_seq > 0 || metadata.block_count > 0;
  };

  const auto normalize_metadata_strings = [](core::model::MailBlockMetadata metadata) {
    metadata.subject = decode_quoted_printable(metadata.subject);
    metadata.owner = decode_quoted_printable(metadata.owner);
    metadata.local_path = decode_quoted_printable(metadata.local_path);
    metadata.mail_folder = decode_quoted_printable(metadata.mail_folder);
    return metadata;
  };

  const auto try_parse = [&](const std::string& candidate) -> std::optional<core::model::MailBlockMetadata> {
    try {
      auto metadata = normalize_metadata_strings(core::model::MailBlockMetadata::from_serialized_text(candidate));
      if (is_meaningful(metadata)) {
        return metadata;
      }
    } catch (const std::exception&) {
    }
    return std::nullopt;
  };

  const std::string original(raw_text);
  const auto quoted_printable = decode_quoted_printable(raw_text);
  if (quoted_printable != original) {
    if (const auto parsed = try_parse(quoted_printable)) {
      return *parsed;
    }
  }

  if (const auto parsed = try_parse(original)) {
    return *parsed;
  }

  const auto base64 = decode_base64(raw_text);
  if (!base64.empty()) {
    if (const auto parsed = try_parse(base64)) {
      return *parsed;
    }

    const auto qp_from_base64 = decode_quoted_printable(base64);
    if (qp_from_base64 != base64) {
      if (const auto parsed = try_parse(qp_from_base64)) {
        return *parsed;
      }
    }
  }

  throw std::runtime_error("metadata is incomplete or unsupported");
}

std::vector<unsigned char> read_block(std::ifstream& input, std::size_t max_bytes) {
  std::vector<unsigned char> data(max_bytes);
  input.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(max_bytes));
  data.resize(static_cast<std::size_t>(input.gcount()));
  return data;
}

std::string normalize_slashes(std::string text) {
  std::replace(text.begin(), text.end(), '\\', '/');
  return text;
}

core::model::MailBlockMetadata extract_metadata_from_message(const std::string& raw_message) {
  const auto message = core::mime::MimeMessage::parse(raw_message);
  for (const auto& part : message.parts) {
    const auto it = part.headers.find("Content-Type");
    if (it == part.headers.end()) {
      continue;
    }
    if (it->second.find("application/json") != std::string::npos || it->second.find("text/plain") != std::string::npos) {
      const std::string metadata_text(part.body.begin(), part.body.end());
      auto metadata = parse_cached_metadata_text(metadata_text);
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
                          const std::string& attachment_name,
                          const std::vector<unsigned char>& payload) {
  core::mime::MimeMessage message;
  message.headers["From"] = "\"" + email_name + "\" <" + username + ">";
  message.headers["To"] = "\"" + email_name + "\" <" + username + ">";
  message.headers["Subject"] = metadata.subject;
  message.headers["Date"] = now_rfc2822_utc();

  core::mime::MimePart metadata_part;
  metadata_part.headers["Content-Type"] = "text/plain; charset=utf-8";
  metadata_part.headers["Content-Transfer-Encoding"] = "quoted-printable";
  const auto metadata_text = metadata.to_legacy_text();
  metadata_part.body.assign(metadata_text.begin(), metadata_text.end());

  core::mime::MimePart file_part;
  file_part.headers["Content-Type"] = "text/plain";
  file_part.headers["Content-Transfer-Encoding"] = "base64";
  file_part.headers["Content-Disposition"] = "attachment; filename=\"" + attachment_name + "\"";
  file_part.body = payload;

  message.parts.push_back(std::move(metadata_part));
  message.parts.push_back(std::move(file_part));
  return message.render_multipart_mixed(core::mime::make_boundary());
}

bool is_cacheable_metadata(const core::model::MailBlockMetadata& metadata) {
  return !metadata.subject.empty() && !metadata.local_path.empty() && metadata.block_seq > 0 && metadata.block_count > 0;
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

  infra::logging::log_info("service", "connecting to IMAP server " + config_.imap_host + ":" + std::to_string(config_.imap_port));
  auto credentials = load_credentials();
  username_ = std::move(credentials.first);
  password_ = std::move(credentials.second);
  transport_.connect(config_, username_, password_);
  connected_ = true;
  infra::logging::log_info("service", "IMAP session established for user " + username_);
}

void MailfsService::disconnect() noexcept {
  transport_.disconnect();
  connected_ = false;
  infra::logging::log_info("service", "IMAP session closed");
}

std::vector<std::string> MailfsService::list_mailboxes() {
  ensure_connected();
  infra::logging::log_debug("service", "listing mailboxes with prefix " + config_.mailbox_prefix);
  auto mailboxes = transport_.list_mailboxes(config_.mailbox_prefix.empty() ? "*" : config_.mailbox_prefix);

  std::vector<std::string> filtered;
  for (const auto& mailbox : mailboxes) {
    if (config_.is_allowed_mailbox(mailbox)) {
      filtered.push_back(mailbox);
    }
  }
  return filtered;
}

std::size_t MailfsService::cache_mailbox(const std::string& mailbox, CacheProgressCallback progress) {
  ensure_mailbox_selected(mailbox);
  infra::logging::log_info("service", "caching mailbox " + mailbox);

  const auto all_uids = transport_.search_all_uids();
  const auto cached_uids = repository_.get_cached_uids(mailbox);
  const auto total = all_uids.size();

  std::vector<std::uint64_t> uncached_uids;
  for (const auto uid : all_uids) {
    if (cached_uids.find(uid) == cached_uids.end()) {
      uncached_uids.push_back(uid);
    }
  }

  const auto cached_count = total - uncached_uids.size();
  if (progress) {
    progress(cached_count, total);
  }

  if (uncached_uids.empty()) {
    if (progress) {
      progress(total, total);
    }
    return 0;
  }

  std::size_t fetched_count = 0;
  std::size_t processed_count = 0;
  const auto batch_size = std::max<std::size_t>(1, config_.cache_fetch_batch_size);
  for (std::size_t offset = 0; offset < uncached_uids.size(); offset += batch_size) {
    const auto end = std::min(offset + batch_size, uncached_uids.size());
    std::vector<std::uint64_t> batch(uncached_uids.begin() + static_cast<std::ptrdiff_t>(offset),
                                     uncached_uids.begin() + static_cast<std::ptrdiff_t>(end));
    const auto metadata_entries = transport_.fetch_metadata(batch);
    for (const auto& entry : metadata_entries) {
      core::model::MailBlockMetadata metadata;
      try {
        metadata = parse_cached_metadata_text(entry.metadata_text);
        if (metadata.encrypted) {
          metadata.local_path = core::security::decrypt_string(metadata.local_path);
        }
      } catch (const std::exception& ex) {
        infra::logging::log_warn("service",
                                 "skipping uid " + std::to_string(entry.uid) + " while caching " + mailbox + ": " + ex.what());
        continue;
      }
      if (!is_cacheable_metadata(metadata)) {
        infra::logging::log_warn("service",
                                 "skipping uid " + std::to_string(entry.uid) +
                                     " because metadata is incomplete or unsupported");
        continue;
      }
      if (metadata.mail_folder.empty()) {
        metadata.mail_folder = mailbox;
      }
      repository_.upsert_mail_block(entry.uid, metadata);
      ++fetched_count;
    }
    processed_count += batch.size();
    if (progress) {
      progress(cached_count + processed_count, total);
    }
  }

  infra::logging::log_info("service",
                           "mailbox " + mailbox + " fetched " + std::to_string(fetched_count) + " uncached messages");
  return fetched_count;
}

std::vector<core::model::CachedFileRecord> MailfsService::list_cached_files(const std::string& mailbox,
                                                                            ListProgressCallback progress) {
  auto files = repository_.list_files(mailbox);
  if (progress) {
    const auto total = files.size();
    for (std::size_t i = 0; i < files.size(); ++i) {
      progress(i + 1, total, files[i].local_path);
    }
  }
  return files;
}

void MailfsService::delete_message_uid(const std::string& mailbox, std::uint64_t uid) {
  ensure_mailbox_selected(mailbox);
  infra::logging::log_warn("service", "deleting uid " + std::to_string(uid) + " from mailbox " + mailbox);
  transport_.delete_message_by_uid(uid);
  repository_.remove_message_uid(mailbox, uid);
}

void MailfsService::upload_file(const std::string& mailbox,
                                const std::filesystem::path& local_file,
                                BlockProgressCallback progress) {
  ensure_mailbox_selected(mailbox);
  infra::logging::log_info("service", "uploading " + local_file.u8string() + " to " + mailbox);

  if (!std::filesystem::exists(local_file)) {
    throw std::runtime_error("local file not found: " + local_file.u8string());
  }

  const auto file_size = std::filesystem::file_size(local_file);
  const auto block_size = config_.block_size_for_file(local_file);
  const auto block_count =
      static_cast<std::int32_t>((file_size + block_size - 1) / block_size);
  const auto file_md5 = core::md5_hex(local_file);
  const auto local_path_text = std::filesystem::absolute(local_file).lexically_normal().u8string();
  const auto encrypted = core::security::should_encrypt_mailbox(mailbox);
  const auto subject_name = encrypted
                                ? core::security::encrypt_string(local_file.filename().u8string())
                                : local_file.filename().u8string();
  const auto attachment_name = subject_name;
  const auto stored_local_path = encrypted ? core::security::encrypt_string(local_path_text) : local_path_text;

  std::ifstream input(local_file, std::ios::binary);
  if (!input) {
    throw std::runtime_error("failed to open file for upload: " + local_file.u8string());
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
    metadata.owner = config_.owner_name;
    metadata.local_path = stored_local_path;
    metadata.mail_folder = mailbox;
    metadata.block_seq = block_seq;
    metadata.block_count = block_count;
    metadata.encrypted = encrypted;

    transport_.append_message(mailbox, build_message(metadata, username_, config_.email_name, attachment_name, payload));
    if (progress) {
      progress(block_seq, block_count, local_file.filename().u8string());
    }
  }

  infra::logging::log_info("service",
                           "upload complete for " + local_file.u8string() + ", blocks=" + std::to_string(block_count));
}

std::filesystem::path MailfsService::download_file(const std::string& mailbox,
                                                   const std::string& local_path,
                                                   BlockProgressCallback progress) {
  ensure_mailbox_selected(mailbox);
  infra::logging::log_info("service", "downloading " + mailbox + ":" + local_path);

  auto cached = repository_.find_file(mailbox, local_path);
  if (!cached.has_value()) {
    cache_mailbox(mailbox);
    cached = repository_.find_file(mailbox, local_path);
  }

  if (!cached.has_value()) {
    throw std::runtime_error("cached file index not found for local path: " + local_path);
  }

  auto file_record = *cached;
  file_record.sort_blocks();
  if (file_record.blocks.size() != static_cast<std::size_t>(file_record.block_count)) {
    throw std::runtime_error("cached file is incomplete for local path: " + local_path);
  }

  const auto save_path = resolve_download_path(file_record.local_path);
  const auto save_tmp_path = save_path.parent_path() / (save_path.filename().u8string() + ".tmp");
  const auto cache_dir = save_path.parent_path() / std::filesystem::u8path("mailfscache_" + file_record.file_md5);
  const auto file_name = save_path.filename().u8string();

  if (std::filesystem::exists(save_path)) {
    infra::logging::log_info("service", "download target already exists: " + save_path.u8string());
    return save_path;
  }

  std::filesystem::create_directories(save_path.parent_path());
  std::filesystem::remove(save_tmp_path);
  std::filesystem::create_directories(cache_dir);

  const auto total_blocks = static_cast<std::int64_t>(file_record.blocks.size());
  for (std::size_t idx = 0; idx < file_record.blocks.size(); ++idx) {
    const auto& block = file_record.blocks[idx];
    const auto cache_block_path = cache_dir / std::filesystem::u8path(std::to_string(block.block_seq));
    if (std::filesystem::exists(cache_block_path)) {
      if (progress) {
        progress(static_cast<std::int64_t>(idx + 1), total_blocks, file_name);
      }
      continue;
    }

    const auto fetched_messages = transport_.fetch_messages({block.uid});
    if (fetched_messages.empty()) {
      throw std::runtime_error("missing downloaded block uid: " + std::to_string(block.uid));
    }

    const auto metadata = extract_metadata_from_message(fetched_messages.front().raw_message);
    const auto payload = extract_attachment_from_message(fetched_messages.front().raw_message);
    if (core::md5_hex(payload) != block.block_md5) {
      throw std::runtime_error("block md5 not match for uid: " + std::to_string(block.uid));
    }
    if (normalize_slashes(metadata.mail_folder) != normalize_slashes(file_record.mail_folder)) {
      throw std::runtime_error("mailfolder not match for uid: " + std::to_string(block.uid));
    }
    if (normalize_slashes(metadata.local_path) != normalize_slashes(file_record.local_path)) {
      throw std::runtime_error("localpath not match for uid: " + std::to_string(block.uid));
    }

    const auto tmp_block_path = cache_block_path.parent_path() / std::filesystem::u8path(cache_block_path.filename().u8string() + ".tmp");
    {
      std::ofstream tmp_block(tmp_block_path, std::ios::binary | std::ios::trunc);
      if (!tmp_block) {
        throw std::runtime_error("failed to open temp block file: " + tmp_block_path.u8string());
      }
      tmp_block.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
    }
    std::filesystem::rename(tmp_block_path, cache_block_path);

    if (progress) {
      progress(static_cast<std::int64_t>(idx + 1), total_blocks, file_name);
    }
  }

  {
    std::ofstream output(save_tmp_path, std::ios::binary | std::ios::trunc);
    if (!output) {
      throw std::runtime_error("failed to open output file: " + save_tmp_path.u8string());
    }
    for (const auto& block : file_record.blocks) {
      const auto cache_block_path = cache_dir / std::filesystem::u8path(std::to_string(block.block_seq));
      std::ifstream input(cache_block_path, std::ios::binary);
      if (!input) {
        throw std::runtime_error("failed to open cached block: " + cache_block_path.u8string());
      }
      output << input.rdbuf();
    }
  }

  if (core::md5_hex(save_tmp_path) != file_record.file_md5) {
    throw std::runtime_error("file md5 not match for local path: " + local_path);
  }

  std::filesystem::rename(save_tmp_path, save_path);
  std::filesystem::remove_all(cache_dir);
  infra::logging::log_info("service",
                           "download complete for " + local_path + ", blocks=" + std::to_string(file_record.block_count));
  return save_path;
}

void MailfsService::ensure_connected() {
  if (!connected_) {
    connect();
  }
}

void MailfsService::ensure_mailbox_selected(const std::string& mailbox) {
  ensure_connected();
  infra::logging::log_debug("service", "selecting mailbox " + mailbox);
  transport_.select_mailbox(mailbox);
}

std::pair<std::string, std::string> MailfsService::load_credentials() const {
  std::ifstream input(std::filesystem::u8path(config_.credential_file));
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

std::filesystem::path MailfsService::resolve_download_path(const std::string& local_path) const {
  auto normalized = normalize_slashes(local_path);
  if (normalized.size() >= 2 && normalized[1] == ':') {
    normalized = std::string(1, normalized[0]) + normalized.substr(2);
  }
  while (!normalized.empty() && normalized.front() == '/') {
    normalized.erase(normalized.begin());
  }
  return std::filesystem::u8path(config_.download_dir) / std::filesystem::u8path(normalized);
}

}  // namespace mailfs::application
