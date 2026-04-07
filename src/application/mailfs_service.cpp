#include "mailfs/application/mailfs_service.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>
#include <tuple>

#include <nlohmann/json.hpp>

#include "mailfs/core/hash.hpp"
#include "mailfs/core/mime/mime_message.hpp"
#include "mailfs/core/model/mail_block_metadata.hpp"
#include "mailfs/core/security/xor_codec.hpp"
#include "mailfs/infra/logging/logger.hpp"

namespace mailfs::application {

namespace {

constexpr std::string_view kOtherFilesMailboxRoot = u8"其他文件夹";

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

std::vector<std::filesystem::path> collect_upload_files(const std::filesystem::path& local_path,
                                                        const core::model::AppConfig& config) {
  std::vector<std::filesystem::path> files;
  if (std::filesystem::is_regular_file(local_path)) {
    if (!config.should_ignore_file(local_path)) {
      files.push_back(local_path);
    }
    return files;
  }

  if (!std::filesystem::is_directory(local_path)) {
    throw std::runtime_error("local path is neither a file nor a directory: " + local_path.u8string());
  }

  for (const auto& entry : std::filesystem::recursive_directory_iterator(local_path)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    if (config.should_ignore_file(entry.path())) {
      continue;
    }
    files.push_back(entry.path());
  }

  std::sort(files.begin(), files.end());
  return files;
}

std::string normalize_slashes(std::string text) {
  std::replace(text.begin(), text.end(), '\\', '/');
  return text;
}

bool starts_with(const std::string& text, const std::string& prefix) {
  return text.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), text.begin());
}

std::string normalize_path_for_compare(std::string text) {
  text = normalize_slashes(std::move(text));
  while (!text.empty() && text.front() == '/') {
    text.erase(text.begin());
  }
  while (!text.empty() && text.back() == '/') {
    text.pop_back();
  }
  return text;
}

std::string last_path_segment(std::string text) {
  text = normalize_path_for_compare(std::move(text));
  const auto slash = text.find_last_of('/');
  if (slash == std::string::npos) {
    return text;
  }
  return text.substr(slash + 1);
}

bool path_matches_prefix(const std::string& local_path, const std::string& prefix) {
  const auto normalized_path = normalize_path_for_compare(local_path);
  const auto normalized_prefix = normalize_path_for_compare(prefix);
  if (normalized_prefix.empty()) {
    return true;
  }
  if (normalized_path == normalized_prefix) {
    return true;
  }
  return starts_with(normalized_path, normalized_prefix) &&
         normalized_path.size() > normalized_prefix.size() &&
         normalized_path[normalized_prefix.size()] == '/';
}

std::string make_playlist_relative_path(const std::string& local_path, const std::string& prefix) {
  auto normalized_path = normalize_path_for_compare(local_path);
  const auto normalized_prefix = normalize_path_for_compare(prefix);
  if (normalized_prefix.empty() || !path_matches_prefix(local_path, prefix)) {
    return normalized_path;
  }
  if (normalized_path == normalized_prefix) {
    return last_path_segment(normalized_path);
  }

  normalized_path.erase(0, normalized_prefix.size());
  while (!normalized_path.empty() && normalized_path.front() == '/') {
    normalized_path.erase(normalized_path.begin());
  }
  return normalized_path.empty() ? last_path_segment(local_path) : normalized_path;
}

std::string encode_base64_url(std::string_view text) {
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

  std::string encoded;
  encoded.reserve(((text.size() + 2) / 3) * 4);

  for (std::size_t i = 0; i < text.size(); i += 3) {
    const auto remaining = std::min<std::size_t>(3, text.size() - i);
    std::uint32_t block = static_cast<unsigned char>(text[i]) << 16;
    if (remaining > 1) {
      block |= static_cast<std::uint32_t>(static_cast<unsigned char>(text[i + 1])) << 8;
    }
    if (remaining > 2) {
      block |= static_cast<std::uint32_t>(static_cast<unsigned char>(text[i + 2]));
    }

    encoded.push_back(kAlphabet[(block >> 18) & 0x3F]);
    encoded.push_back(kAlphabet[(block >> 12) & 0x3F]);
    encoded.push_back(remaining > 1 ? kAlphabet[(block >> 6) & 0x3F] : '=');
    encoded.push_back(remaining > 2 ? kAlphabet[block & 0x3F] : '=');
  }

  return encoded;
}

std::string trim_trailing_slashes(std::string text) {
  while (!text.empty() && text.back() == '/') {
    text.pop_back();
  }
  return text;
}

std::string build_http_stream_url(const std::string& http_copy_addr,
                                  const std::string& mailbox,
                                  const std::string& local_path) {
  return trim_trailing_slashes(http_copy_addr) + "/httptoimap?imapdir=" + encode_base64_url(mailbox) +
         "&localpath=" + encode_base64_url(local_path);
}

bool has_complete_blocks(core::model::CachedFileRecord file_record) {
  file_record.sort_blocks();
  if (file_record.block_count <= 0 || file_record.blocks.size() != static_cast<std::size_t>(file_record.block_count)) {
    return false;
  }

  for (std::int32_t expected_seq = 1; expected_seq <= file_record.block_count; ++expected_seq) {
    const auto& block = file_record.blocks[static_cast<std::size_t>(expected_seq - 1)];
    if (block.block_seq != expected_seq || block.uid == 0 || block.block_md5.empty()) {
      return false;
    }
  }
  return true;
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

struct RemoteBlockSnapshot {
  std::uint64_t uid = 0;
  core::model::MailBlockMetadata metadata;
};

struct VersionKey {
  std::string file_md5;
  std::uint64_t file_size = 0;
  std::int32_t block_count = 0;

  bool operator<(const VersionKey& other) const {
    return std::tie(file_md5, file_size, block_count) < std::tie(other.file_md5, other.file_size, other.block_count);
  }
};

struct VersionCandidate {
  VersionKey key;
  std::map<std::int32_t, RemoteBlockSnapshot> best_blocks_by_seq;
  std::vector<std::uint64_t> duplicate_uids;

  [[nodiscard]] bool is_complete() const {
    if (key.block_count <= 0 || best_blocks_by_seq.size() != static_cast<std::size_t>(key.block_count)) {
      return false;
    }
    for (std::int32_t seq = 1; seq <= key.block_count; ++seq) {
      const auto it = best_blocks_by_seq.find(seq);
      if (it == best_blocks_by_seq.end() || it->second.uid == 0) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] std::uint64_t max_uid() const {
    std::uint64_t max_uid_value = 0;
    for (const auto& [seq, block] : best_blocks_by_seq) {
      static_cast<void>(seq);
      max_uid_value = std::max(max_uid_value, block.uid);
    }
    return max_uid_value;
  }

  [[nodiscard]] std::vector<std::uint64_t> kept_uids() const {
    std::vector<std::uint64_t> result;
    result.reserve(best_blocks_by_seq.size());
    for (const auto& [seq, block] : best_blocks_by_seq) {
      static_cast<void>(seq);
      result.push_back(block.uid);
    }
    std::sort(result.begin(), result.end());
    return result;
  }
};

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
  const auto list_pattern = std::string(kOtherFilesMailboxRoot) + "/%";
  infra::logging::log_debug("service", "listing mailboxes under " + std::string(kOtherFilesMailboxRoot));
  auto mailboxes = transport_.list_mailboxes(list_pattern);

  std::vector<std::string> filtered;
  for (const auto& mailbox : mailboxes) {
    if (!starts_with(mailbox, std::string(kOtherFilesMailboxRoot) + "/")) {
      continue;
    }
    if (core::security::should_encrypt_mailbox(mailbox)) {
      continue;
    }
    if (config_.is_allowed_mailbox(mailbox)) {
      filtered.push_back(mailbox);
    }
  }
  std::sort(filtered.begin(), filtered.end());
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

std::vector<IntegrityCheckResult> MailfsService::check_cached_integrity(const std::string& mailbox,
                                                                        const std::string& local_path_prefix) {
  auto files = repository_.list_files(mailbox);
  std::vector<IntegrityCheckResult> results;
  results.reserve(files.size());

  for (auto& file : files) {
    if (!path_matches_prefix(file.local_path, local_path_prefix)) {
      continue;
    }

    file.sort_blocks();
    IntegrityCheckResult result;
    result.cached_blocks = static_cast<std::int64_t>(file.blocks.size());
    result.expected_blocks = file.block_count;
    result.ok = has_complete_blocks(file);
    result.file = std::move(file);
    results.push_back(std::move(result));
  }

  return results;
}

std::vector<DeduplicationResult> MailfsService::deduplicate_mailbox(const std::string& mailbox,
                                                                    const std::string& local_path_prefix,
                                                                    CacheProgressCallback progress) {
  ensure_mailbox_selected(mailbox);
  infra::logging::log_info("service", "deduplicating mailbox " + mailbox);

  const auto all_uids = transport_.search_all_uids();
  const auto total = all_uids.size();
  if (progress) {
    progress(0, total);
  }

  std::map<std::string, std::map<VersionKey, VersionCandidate>> groups_by_path;
  const auto batch_size = std::max<std::size_t>(1, config_.cache_fetch_batch_size);
  std::size_t processed_count = 0;
  for (std::size_t offset = 0; offset < all_uids.size(); offset += batch_size) {
    const auto end = std::min(offset + batch_size, all_uids.size());
    std::vector<std::uint64_t> batch(all_uids.begin() + static_cast<std::ptrdiff_t>(offset),
                                     all_uids.begin() + static_cast<std::ptrdiff_t>(end));
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
                                 "skipping uid " + std::to_string(entry.uid) + " while deduplicating " + mailbox +
                                     ": " + ex.what());
        continue;
      }
      if (!is_cacheable_metadata(metadata)) {
        continue;
      }
      if (metadata.mail_folder.empty()) {
        metadata.mail_folder = mailbox;
      }
      if (!path_matches_prefix(metadata.local_path, local_path_prefix)) {
        continue;
      }

      VersionKey key{metadata.file_md5, metadata.file_size, metadata.block_count};
      auto& candidate = groups_by_path[metadata.local_path][key];
      candidate.key = key;

      const auto block_it = candidate.best_blocks_by_seq.find(metadata.block_seq);
      if (block_it == candidate.best_blocks_by_seq.end()) {
        candidate.best_blocks_by_seq.emplace(metadata.block_seq, RemoteBlockSnapshot{entry.uid, metadata});
      } else if (entry.uid > block_it->second.uid) {
        candidate.duplicate_uids.push_back(block_it->second.uid);
        block_it->second = RemoteBlockSnapshot{entry.uid, metadata};
      } else {
        candidate.duplicate_uids.push_back(entry.uid);
      }
    }
    processed_count += batch.size();
    if (progress) {
      progress(processed_count, total);
    }
  }

  std::vector<DeduplicationResult> results;
  std::vector<std::uint64_t> deleted_uids;
  for (auto& [local_path, versions] : groups_by_path) {
    if (versions.size() <= 1) {
      auto only = versions.begin();
      if (!only->second.duplicate_uids.empty()) {
        DeduplicationResult result;
        result.local_path = local_path;
        result.kept_file_md5 = only->second.key.file_md5;
        result.kept_uids = only->second.kept_uids();
        result.deleted_uids = only->second.duplicate_uids;
        std::sort(result.deleted_uids.begin(), result.deleted_uids.end());
        deleted_uids.insert(deleted_uids.end(), result.deleted_uids.begin(), result.deleted_uids.end());
        results.push_back(std::move(result));
      }
      continue;
    }

    auto keep_it = versions.begin();
    for (auto it = versions.begin(); it != versions.end(); ++it) {
      const auto keep_score = std::make_pair(keep_it->second.is_complete(), keep_it->second.max_uid());
      const auto current_score = std::make_pair(it->second.is_complete(), it->second.max_uid());
      if (current_score > keep_score) {
        keep_it = it;
      }
    }

    DeduplicationResult result;
    result.local_path = local_path;
    result.kept_file_md5 = keep_it->second.key.file_md5;
    result.kept_uids = keep_it->second.kept_uids();
    result.deleted_uids = keep_it->second.duplicate_uids;

    for (auto it = versions.begin(); it != versions.end(); ++it) {
      if (it == keep_it) {
        continue;
      }
      const auto kept = it->second.kept_uids();
      result.deleted_uids.insert(result.deleted_uids.end(), kept.begin(), kept.end());
      result.deleted_uids.insert(result.deleted_uids.end(),
                                 it->second.duplicate_uids.begin(),
                                 it->second.duplicate_uids.end());
    }

    std::sort(result.deleted_uids.begin(), result.deleted_uids.end());
    result.deleted_uids.erase(std::unique(result.deleted_uids.begin(), result.deleted_uids.end()), result.deleted_uids.end());
    if (!result.deleted_uids.empty()) {
      deleted_uids.insert(deleted_uids.end(), result.deleted_uids.begin(), result.deleted_uids.end());
      results.push_back(std::move(result));
    }
  }

  std::sort(deleted_uids.begin(), deleted_uids.end());
  deleted_uids.erase(std::unique(deleted_uids.begin(), deleted_uids.end()), deleted_uids.end());
  for (const auto uid : deleted_uids) {
    infra::logging::log_warn("service", "dedup deleting uid " + std::to_string(uid) + " from " + mailbox);
    transport_.delete_message_by_uid(uid);
  }

  if (!deleted_uids.empty()) {
    repository_.clear_mailbox(mailbox);
    cache_mailbox(mailbox);
  }

  std::sort(results.begin(), results.end(), [](const DeduplicationResult& lhs, const DeduplicationResult& rhs) {
    return lhs.local_path < rhs.local_path;
  });
  infra::logging::log_info("service",
                           "dedup complete for " + mailbox + ", deleted_uids=" + std::to_string(deleted_uids.size()));
  return results;
}

std::string MailfsService::export_playlist_json(const std::string& mailbox, const std::string& local_path_prefix) {
  const auto files = repository_.list_files(mailbox);
  nlohmann::json root = nlohmann::json::object();

  for (const auto& file : files) {
    if (!path_matches_prefix(file.local_path, local_path_prefix)) {
      continue;
    }

    const auto relative_path = make_playlist_relative_path(file.local_path, local_path_prefix);
    if (relative_path.empty()) {
      continue;
    }

    auto* node = &root;
    std::stringstream path_stream(relative_path);
    std::string segment;
    std::vector<std::string> parts;
    while (std::getline(path_stream, segment, '/')) {
      if (!segment.empty()) {
        parts.push_back(segment);
      }
    }
    if (parts.empty()) {
      continue;
    }

    bool path_conflicted = false;
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
      auto& child = (*node)[parts[i]];
      if (child.is_null()) {
        child = nlohmann::json::object();
      } else if (!child.is_object()) {
        path_conflicted = true;
        break;
      }
      node = &child;
    }
    if (path_conflicted) {
      continue;
    }

    auto& leaf = (*node)[parts.back()];
    if (leaf.is_object()) {
      continue;
    }
    leaf = build_http_stream_url(config_.http_copy_addr, mailbox, file.local_path);
  }

  return root.dump(2);
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
  auto existing = repository_.find_file(mailbox, local_path_text);
  if (!existing.has_value() && encrypted) {
    existing = repository_.find_file(mailbox, stored_local_path);
  }
  if (existing.has_value()) {
    infra::logging::log_info("service",
                             "skip upload because file already exists in cache index for " + mailbox + ":" + local_path_text);
    return;
  }

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

    const auto appended_uid =
        transport_.append_message(mailbox, build_message(metadata, username_, config_.email_name, attachment_name, payload));
    if (appended_uid.has_value()) {
      repository_.upsert_mail_block(*appended_uid, metadata);
    }
    if (progress) {
      progress(block_seq, block_count, local_file.filename().u8string());
    }
  }

  infra::logging::log_info("service",
                           "upload complete for " + local_file.u8string() + ", blocks=" + std::to_string(block_count));
}

std::size_t MailfsService::upload_path(const std::string& mailbox,
                                       const std::filesystem::path& local_path,
                                       BlockProgressCallback progress) {
  const auto files = collect_upload_files(local_path, config_);
  if (files.empty()) {
    throw std::runtime_error("no uploadable files found under path: " + local_path.u8string());
  }

  for (const auto& file : files) {
    upload_file(mailbox, file, progress);
  }
  return files.size();
}

std::filesystem::path MailfsService::download_file(const std::string& mailbox,
                                                   const std::string& local_path,
                                                   BlockProgressCallback progress) {
  infra::logging::log_info("service", "downloading " + mailbox + ":" + local_path);
  auto file_record = resolve_cached_file(mailbox, local_path);

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

    const auto downloaded = fetch_cached_block(mailbox, file_record, block);

    const auto tmp_block_path = cache_block_path.parent_path() / std::filesystem::u8path(cache_block_path.filename().u8string() + ".tmp");
    {
      std::ofstream tmp_block(tmp_block_path, std::ios::binary | std::ios::trunc);
      if (!tmp_block) {
        throw std::runtime_error("failed to open temp block file: " + tmp_block_path.u8string());
      }
      tmp_block.write(reinterpret_cast<const char*>(downloaded.payload.data()),
                      static_cast<std::streamsize>(downloaded.payload.size()));
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

core::model::CachedFileRecord MailfsService::resolve_cached_file(const std::string& mailbox,
                                                                 const std::string& local_path) {
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
  return file_record;
}

DownloadedBlockData MailfsService::fetch_cached_block(const std::string& mailbox,
                                                      const core::model::CachedFileRecord& file_record,
                                                      const core::model::CachedBlockRecord& block) {
  ensure_mailbox_selected(mailbox);

  const auto fetched_messages = transport_.fetch_messages({block.uid});
  if (fetched_messages.empty()) {
    throw std::runtime_error("missing downloaded block uid: " + std::to_string(block.uid));
  }

  auto metadata = extract_metadata_from_message(fetched_messages.front().raw_message);
  auto payload = extract_attachment_from_message(fetched_messages.front().raw_message);
  if (core::md5_hex(payload) != block.block_md5) {
    throw std::runtime_error("block md5 not match for uid: " + std::to_string(block.uid));
  }
  if (normalize_slashes(metadata.mail_folder) != normalize_slashes(file_record.mail_folder)) {
    throw std::runtime_error("mailfolder not match for uid: " + std::to_string(block.uid));
  }
  if (normalize_slashes(metadata.local_path) != normalize_slashes(file_record.local_path)) {
    throw std::runtime_error("localpath not match for uid: " + std::to_string(block.uid));
  }

  return {std::move(metadata), std::move(payload)};
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
