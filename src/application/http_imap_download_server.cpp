#include "mailfs/application/http_imap_download_server.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <list>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <thread>

#include "mailfs/infra/imap/imap_client.hpp"
#include "mailfs/infra/logging/logger.hpp"
#include "mailfs/infra/storage/sqlite_cache_repository.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace mailfs::application {

namespace {

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;
#endif

constexpr std::size_t kMaxRequestBytes = 64 * 1024;
constexpr std::size_t kDefaultBlockCacheEntries = 32;

std::string lower_copy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

void close_socket(NativeSocket socket) {
#ifdef _WIN32
  closesocket(socket);
#else
  close(socket);
#endif
}

std::string trim_copy(std::string value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
    value.erase(value.begin());
  }
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
    value.pop_back();
  }
  return value;
}

std::optional<std::string> get_header_value(const std::unordered_map<std::string, std::string>& headers,
                                            std::string_view name) {
  const auto expected = lower_copy(std::string(name));
  for (const auto& [key, value] : headers) {
    if (lower_copy(key) == expected) {
      return value;
    }
  }
  return std::nullopt;
}

bool send_all(NativeSocket socket, std::string_view data) {
  std::size_t sent = 0;
  while (sent < data.size()) {
#ifdef _WIN32
    const auto count = send(socket, data.data() + static_cast<std::ptrdiff_t>(sent),
                            static_cast<int>(data.size() - sent), 0);
#else
    const auto count = send(socket, data.data() + static_cast<std::ptrdiff_t>(sent), data.size() - sent, 0);
#endif
    if (count <= 0) {
      return false;
    }
    sent += static_cast<std::size_t>(count);
  }
  return true;
}

std::pair<std::string, std::string> split_host_port(const std::string& listen_addr) {
  if (listen_addr.empty()) {
    throw std::runtime_error("http_listen_addr is empty");
  }

  if (listen_addr.front() == '[') {
    const auto close = listen_addr.find(']');
    if (close == std::string::npos || close + 1 >= listen_addr.size() || listen_addr[close + 1] != ':') {
      throw std::runtime_error("invalid listen address: " + listen_addr);
    }
    return {listen_addr.substr(1, close - 1), listen_addr.substr(close + 2)};
  }

  const auto colon = listen_addr.rfind(':');
  if (colon == std::string::npos) {
    throw std::runtime_error("listen address must include port: " + listen_addr);
  }

  return {listen_addr.substr(0, colon), listen_addr.substr(colon + 1)};
}

std::string percent_decode(std::string_view text) {
  auto decode_hex = [](char ch) -> int {
    if (ch >= '0' && ch <= '9') {
      return ch - '0';
    }
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (ch >= 'a' && ch <= 'f') {
      return ch - 'a' + 10;
    }
    return -1;
  };

  std::string decoded;
  decoded.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '%' && i + 2 < text.size()) {
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

std::unordered_map<std::string, std::string> parse_query_string(std::string_view query) {
  std::unordered_map<std::string, std::string> values;
  while (!query.empty()) {
    const auto amp = query.find('&');
    const auto pair = query.substr(0, amp);
    const auto equal = pair.find('=');
    const auto key = percent_decode(pair.substr(0, equal));
    const auto value = equal == std::string_view::npos ? std::string() : percent_decode(pair.substr(equal + 1));
    values.emplace(key, value);
    if (amp == std::string_view::npos) {
      break;
    }
    query.remove_prefix(amp + 1);
  }
  return values;
}

std::string decode_base64_compact(std::string_view text) {
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

  if (text.empty()) {
    return {};
  }

  std::string padded(text);
  while ((padded.size() % 4) != 0) {
    padded.push_back('=');
  }

  std::string decoded;
  decoded.reserve((padded.size() / 4) * 3);
  for (std::size_t i = 0; i < padded.size(); i += 4) {
    const auto a = decode_char(static_cast<unsigned char>(padded[i]));
    const auto b = decode_char(static_cast<unsigned char>(padded[i + 1]));
    const auto c = decode_char(static_cast<unsigned char>(padded[i + 2]));
    const auto d = decode_char(static_cast<unsigned char>(padded[i + 3]));
    if (a < 0 || b < 0 || c == -1 || d == -1) {
      throw std::runtime_error("invalid base64 parameter");
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

std::string decode_base64_param(std::string value) {
  try {
    return decode_base64_compact(value);
  } catch (const std::exception&) {
    std::replace(value.begin(), value.end(), '-', '+');
    std::replace(value.begin(), value.end(), '_', '/');
    return decode_base64_compact(value);
  }
}

std::vector<std::uint64_t> build_block_offsets(const core::model::CachedFileRecord& file_record,
                                               std::uint64_t fallback_block_size) {
  std::vector<std::uint64_t> offsets(file_record.blocks.size() + 1, 0);
  for (std::size_t i = 0; i < file_record.blocks.size(); ++i) {
    auto block_size = file_record.blocks[i].block_size;
    if (block_size == 0) {
      block_size = fallback_block_size;
    }
    offsets[i + 1] = offsets[i] + block_size;
  }
  if (file_record.file_size > 0 && !offsets.empty()) {
    offsets.back() = file_record.file_size;
  }
  return offsets;
}

std::pair<std::size_t, std::uint64_t> find_block_for_offset(const std::vector<std::uint64_t>& offsets,
                                                            std::uint64_t byte_offset) {
  for (std::size_t i = 0; i + 1 < offsets.size(); ++i) {
    if (byte_offset < offsets[i + 1]) {
      return {i, byte_offset - offsets[i]};
    }
  }
  if (offsets.size() > 1) {
    return {offsets.size() - 2, byte_offset - offsets[offsets.size() - 2]};
  }
  return {0, byte_offset};
}

bool parse_range_header(const std::string& header,
                        std::uint64_t total_size,
                        std::uint64_t& start,
                        std::uint64_t& end) {
  if (header.empty() || header.rfind("bytes=", 0) != 0 || total_size == 0) {
    return false;
  }

  auto spec = header.substr(6);
  const auto comma = spec.find(',');
  if (comma != std::string::npos) {
    spec = spec.substr(0, comma);
  }

  const auto dash = spec.find('-');
  if (dash == std::string::npos) {
    return false;
  }

  const auto start_text = trim_copy(spec.substr(0, dash));
  const auto end_text = trim_copy(spec.substr(dash + 1));

  try {
    if (start_text.empty()) {
      const auto suffix = std::stoull(end_text);
      if (suffix == 0) {
        return false;
      }
      start = suffix >= total_size ? 0 : total_size - suffix;
      end = total_size - 1;
    } else {
      start = std::stoull(start_text);
      end = end_text.empty() ? (total_size - 1) : std::stoull(end_text);
    }
  } catch (const std::exception&) {
    return false;
  }

  if (start > end || start >= total_size) {
    return false;
  }
  if (end >= total_size) {
    end = total_size - 1;
  }
  return true;
}

std::string guess_content_type(const std::string& path) {
  const auto lower = lower_copy(path);
  if (lower.size() >= 4 && lower.substr(lower.size() - 4) == ".mp4") {
    return "video/mp4";
  }
  if (lower.size() >= 4 && lower.substr(lower.size() - 4) == ".mp3") {
    return "audio/mpeg";
  }
  if (lower.size() >= 4 && lower.substr(lower.size() - 4) == ".png") {
    return "image/png";
  }
  if (lower.size() >= 4 && lower.substr(lower.size() - 4) == ".jpg") {
    return "image/jpeg";
  }
  if (lower.size() >= 5 && lower.substr(lower.size() - 5) == ".jpeg") {
    return "image/jpeg";
  }
  if (lower.size() >= 4 && lower.substr(lower.size() - 4) == ".gif") {
    return "image/gif";
  }
  if (lower.size() >= 4 && lower.substr(lower.size() - 4) == ".pdf") {
    return "application/pdf";
  }
  if (lower.size() >= 4 && lower.substr(lower.size() - 4) == ".zip") {
    return "application/zip";
  }
  if (lower.size() >= 4 && lower.substr(lower.size() - 4) == ".txt") {
    return "text/plain; charset=utf-8";
  }
  return "application/octet-stream";
}

HttpRequest parse_http_request_text(const std::string& request_text) {
  std::istringstream input(request_text);
  HttpRequest request;

  std::string request_line;
  if (!std::getline(input, request_line)) {
    throw std::runtime_error("empty http request");
  }
  if (!request_line.empty() && request_line.back() == '\r') {
    request_line.pop_back();
  }

  std::istringstream line_input(request_line);
  std::string version;
  if (!(line_input >> request.method >> request.target >> version)) {
    throw std::runtime_error("malformed request line");
  }

  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      break;
    }
    const auto colon = line.find(':');
    if (colon == std::string::npos) {
      continue;
    }
    request.headers.emplace(trim_copy(line.substr(0, colon)), trim_copy(line.substr(colon + 1)));
  }

  return request;
}

class MailfsServiceDownloadProvider final : public IHttpImapDownloadProvider {
 public:
  explicit MailfsServiceDownloadProvider(core::model::AppConfig config)
      : config_(std::move(config)),
        repository_(std::filesystem::u8path(config_.database_path)),
        service_(config_, client_, repository_) {}

  ~MailfsServiceDownloadProvider() override {
    service_.disconnect();
  }

  core::model::CachedFileRecord resolve_file(const std::string& mailbox, const std::string& local_path) override {
    return service_.resolve_cached_file(mailbox, local_path);
  }

  DownloadedBlockData fetch_block(const std::string& mailbox,
                                  const core::model::CachedFileRecord& file_record,
                                  const core::model::CachedBlockRecord& block) override {
    return service_.fetch_cached_block(mailbox, file_record, block);
  }

 private:
  core::model::AppConfig config_;
  infra::storage::SQLiteCacheRepository repository_;
  infra::imap::ImapClient client_;
  MailfsService service_;
};

struct SocketCloser {
  explicit SocketCloser(NativeSocket value) : socket(value) {}
  ~SocketCloser() {
    if (socket != kInvalidSocket) {
      close_socket(socket);
    }
  }
  NativeSocket socket = kInvalidSocket;
};

}  // namespace

HttpImapDownloadServer::HttpImapDownloadServer(core::model::AppConfig config)
    : config_(std::move(config)) {
  auto cfg = config_;
  provider_factory_ = [cfg = std::move(cfg)]() mutable {
    return std::make_shared<MailfsServiceDownloadProvider>(cfg);
  };
  prefetch_worker_ = std::thread([this]() {
    prefetch_worker_loop();
  });
}

HttpImapDownloadServer::HttpImapDownloadServer(core::model::AppConfig config, DownloadProviderFactory provider_factory)
    : config_(std::move(config)), provider_factory_(std::move(provider_factory)) {
  if (!provider_factory_) {
    throw std::runtime_error("http download provider factory is not configured");
  }
  prefetch_worker_ = std::thread([this]() {
    prefetch_worker_loop();
  });
}

HttpImapDownloadServer::~HttpImapDownloadServer() {
  {
    std::lock_guard<std::mutex> lock(prefetch_mutex_);
    stop_prefetch_worker_ = true;
    prefetch_sessions_.clear();
    prefetch_queue_.clear();
    prefetch_pending_keys_.clear();
  }
  prefetch_cv_.notify_all();
  if (prefetch_worker_.joinable()) {
    prefetch_worker_.join();
  }
}

HttpResponse HttpImapDownloadServer::make_text_response(int status_code, std::string body) const {
  HttpResponse response;
  response.status_code = status_code;
  response.reason = status_code == 200    ? "OK"
                    : status_code == 400 ? "Bad Request"
                    : status_code == 404 ? "Not Found"
                    : status_code == 405 ? "Method Not Allowed"
                    : status_code == 416 ? "Range Not Satisfiable"
                                         : "Internal Server Error";
  response.body = std::move(body);
  response.headers.emplace_back("Content-Type", "text/plain; charset=utf-8");
  response.headers.emplace_back("Content-Length", std::to_string(response.body.size()));
  response.headers.emplace_back("Connection", "close");
  return response;
}

std::string HttpImapDownloadServer::make_block_cache_key(const std::string& mailbox, std::uint64_t uid) const {
  return mailbox + "|" + std::to_string(uid);
}

bool HttpImapDownloadServer::has_cached_block(const std::string& cache_key) const {
  {
    std::lock_guard<std::mutex> lock(block_cache_mutex_);
    return block_cache_.find(cache_key) != block_cache_.end();
  }
}

std::shared_ptr<DownloadedBlockData> HttpImapDownloadServer::load_block_impl(
    const std::shared_ptr<IHttpImapDownloadProvider>& provider,
    const std::string& cache_key,
    const std::string& mailbox,
    const core::model::CachedFileRecord& file_record,
    const core::model::CachedBlockRecord& block) {
  std::shared_ptr<BlockLoadState> load_state;
  bool is_owner = false;

  {
    std::lock_guard<std::mutex> lock(block_cache_mutex_);
    const auto found = block_cache_.find(cache_key);
    if (found != block_cache_.end()) {
      block_cache_order_.splice(block_cache_order_.begin(), block_cache_order_, found->second.order);
      return found->second.data;
    }

    const auto inflight = block_loads_.find(cache_key);
    if (inflight != block_loads_.end()) {
      load_state = inflight->second;
    } else {
      load_state = std::make_shared<BlockLoadState>();
      block_loads_.emplace(cache_key, load_state);
      is_owner = true;
    }
  }

  if (!is_owner) {
    std::unique_lock<std::mutex> wait_lock(load_state->mutex);
    load_state->cv.wait(wait_lock, [&] {
      return load_state->done;
    });
    if (!load_state->data) {
      throw std::runtime_error(load_state->error.empty() ? "block download failed" : load_state->error);
    }
    return load_state->data;
  }

  try {
    auto downloaded = std::make_shared<DownloadedBlockData>(provider->fetch_block(mailbox, file_record, block));
    {
      std::lock_guard<std::mutex> state_lock(load_state->mutex);
      load_state->data = downloaded;
      load_state->done = true;
    }
    load_state->cv.notify_all();

    std::lock_guard<std::mutex> lock(block_cache_mutex_);
    block_loads_.erase(cache_key);
    block_cache_order_.push_front(cache_key);
    block_cache_.emplace(cache_key, BlockCacheEntry{downloaded, block_cache_order_.begin()});
    while (block_cache_.size() > kDefaultBlockCacheEntries) {
      const auto stale = block_cache_order_.back();
      block_cache_order_.pop_back();
      block_cache_.erase(stale);
    }
    return downloaded;
  } catch (const std::exception& ex) {
    {
      std::lock_guard<std::mutex> state_lock(load_state->mutex);
      load_state->error = ex.what();
      load_state->done = true;
    }
    load_state->cv.notify_all();
    std::lock_guard<std::mutex> lock(block_cache_mutex_);
    block_loads_.erase(cache_key);
    throw;
  }
}

std::shared_ptr<DownloadedBlockData> HttpImapDownloadServer::load_block(
    const std::shared_ptr<IHttpImapDownloadProvider>& provider,
    const std::string& mailbox,
    const core::model::CachedFileRecord& file_record,
    const core::model::CachedBlockRecord& block) {
  const auto cache_key = make_block_cache_key(mailbox, block.uid);
  return load_block_impl(provider, cache_key, mailbox, file_record, block);
}

std::uint64_t HttpImapDownloadServer::start_prefetch_session(const std::string& mailbox,
                                                             const core::model::CachedFileRecord& file_record,
                                                             std::size_t first_block_index) {
  if (first_block_index >= file_record.blocks.size()) {
    return 0;
  }

  std::lock_guard<std::mutex> lock(prefetch_mutex_);
  const auto session_id = next_prefetch_session_id_++;
  PrefetchSession session;
  session.mailbox = mailbox;
  session.file_record = file_record;
  session.next_block_index = first_block_index;
  session.allowed_block_index = first_block_index;
  prefetch_sessions_.emplace(session_id, std::move(session));
  enqueue_next_prefetch_locked(session_id);
  return session_id;
}

void HttpImapDownloadServer::stop_prefetch_session(std::uint64_t session_id) {
  if (session_id == 0) {
    return;
  }

  std::lock_guard<std::mutex> lock(prefetch_mutex_);
  prefetch_sessions_.erase(session_id);
  for (auto it = prefetch_queue_.begin(); it != prefetch_queue_.end();) {
    if (it->session_id == session_id) {
      prefetch_pending_keys_.erase(it->cache_key);
      it = prefetch_queue_.erase(it);
      continue;
    }
    ++it;
  }
}

void HttpImapDownloadServer::advance_prefetch_session(std::uint64_t session_id) {
  if (session_id == 0) {
    return;
  }

  std::lock_guard<std::mutex> lock(prefetch_mutex_);
  const auto session_it = prefetch_sessions_.find(session_id);
  if (session_it == prefetch_sessions_.end() || !session_it->second.active) {
    return;
  }

  auto& session = session_it->second;
  if (session.allowed_block_index + 1 < session.file_record.blocks.size()) {
    ++session.allowed_block_index;
  }
  enqueue_next_prefetch_locked(session_id);
}

void HttpImapDownloadServer::enqueue_next_prefetch_locked(std::uint64_t session_id) {
  const auto session_it = prefetch_sessions_.find(session_id);
  if (session_it == prefetch_sessions_.end() || !session_it->second.active) {
    return;
  }

  auto& session = session_it->second;
  if (session.task_inflight_or_queued) {
    return;
  }
  while (session.next_block_index < session.file_record.blocks.size() &&
         session.next_block_index <= session.allowed_block_index) {
    const auto block_index = session.next_block_index++;
    const auto& block = session.file_record.blocks[block_index];
    const auto cache_key = make_block_cache_key(session.mailbox, block.uid);
    if (prefetch_pending_keys_.find(cache_key) != prefetch_pending_keys_.end() || has_cached_block(cache_key)) {
      continue;
    }

    PrefetchTask task;
    task.session_id = session_id;
    task.cache_key = cache_key;
    task.mailbox = session.mailbox;
    task.file_record = session.file_record;
    task.block_index = block_index;
    session.task_inflight_or_queued = true;
    prefetch_pending_keys_.insert(cache_key);
    prefetch_queue_.push_back(std::move(task));
    prefetch_cv_.notify_one();
    break;
  }
}

void HttpImapDownloadServer::prefetch_worker_loop() {
  for (;;) {
    PrefetchTask task;
    {
      std::unique_lock<std::mutex> lock(prefetch_mutex_);
      prefetch_cv_.wait(lock, [&] {
        return stop_prefetch_worker_ || !prefetch_queue_.empty();
      });
      if (stop_prefetch_worker_) {
        return;
      }
      task = std::move(prefetch_queue_.front());
      prefetch_queue_.pop_front();

      const auto session_it = prefetch_sessions_.find(task.session_id);
      if (session_it == prefetch_sessions_.end() || !session_it->second.active) {
        prefetch_pending_keys_.erase(task.cache_key);
        continue;
      }
    }

    try {
      auto provider = provider_factory_();
      const auto& block = task.file_record.blocks[task.block_index];
      load_block(provider, task.mailbox, task.file_record, block);
      infra::logging::log_debug("http",
                                "prefetched block uid=" + std::to_string(block.uid) + " mailbox=" + task.mailbox);
    } catch (const std::exception& ex) {
      infra::logging::log_warn("http",
                               "prefetch failed for block uid=" +
                                   std::to_string(task.file_record.blocks[task.block_index].uid) + ": " + ex.what());
    }

    {
      std::lock_guard<std::mutex> lock(prefetch_mutex_);
      prefetch_pending_keys_.erase(task.cache_key);
      const auto session_it = prefetch_sessions_.find(task.session_id);
      if (session_it != prefetch_sessions_.end()) {
        session_it->second.task_inflight_or_queued = false;
      }
      if (session_it != prefetch_sessions_.end() && session_it->second.active) {
        enqueue_next_prefetch_locked(task.session_id);
      }
    }
  }
}

HttpResponse HttpImapDownloadServer::handle_request(const HttpRequest& request) {
  try {
    if (request.method != "GET") {
      auto response = make_text_response(405, "only GET is supported\n");
      response.headers.emplace_back("Allow", "GET");
      return response;
    }

    const auto query_pos = request.target.find('?');
    const auto path = request.target.substr(0, query_pos);
    if (path != "/httptoimap") {
      return make_text_response(404, "not found\n");
    }

    const auto query = query_pos == std::string::npos ? std::string_view{} : std::string_view(request.target).substr(query_pos + 1);
    const auto params = parse_query_string(query);
    const auto imap_it = params.find("imapdir");
    const auto path_it = params.find("localpath");
    if (imap_it == params.end() || path_it == params.end() || imap_it->second.empty() || path_it->second.empty()) {
      return make_text_response(400, "missing imapdir or localpath parameter\n");
    }

    std::string mailbox;
    std::string local_path;
    try {
      mailbox = decode_base64_param(imap_it->second);
      local_path = decode_base64_param(path_it->second);
    } catch (const std::exception&) {
      return make_text_response(400, "invalid base64 parameter\n");
    }

    auto provider = provider_factory_();
    core::model::CachedFileRecord file_record;
    try {
      file_record = provider->resolve_file(mailbox, local_path);
    } catch (const std::exception& ex) {
      if (std::string(ex.what()).find("cached file index not found") != std::string::npos) {
        return make_text_response(404, "file not found in cache\n");
      }
      throw;
    }

    std::uint64_t total_size = file_record.file_size;
    if (total_size == 0 && !file_record.blocks.empty()) {
      const auto first_block = load_block(provider, mailbox, file_record, file_record.blocks.front());
      total_size = first_block->metadata.file_size;
      if (total_size == 0) {
        total_size = static_cast<std::uint64_t>(first_block->payload.size());
      }
      file_record.file_size = total_size;
    }
    if (total_size == 0) {
      return make_text_response(500, "file size is unavailable in cache\n");
    }

    const auto offsets = build_block_offsets(file_record, config_.default_block_size);
    std::uint64_t range_start = 0;
    std::uint64_t range_end = total_size - 1;
    bool partial = false;
    if (const auto range_header = get_header_value(request.headers, "Range")) {
      if (!parse_range_header(*range_header, total_size, range_start, range_end)) {
        auto response = make_text_response(416, "invalid range\n");
        response.headers.emplace_back("Content-Range", "bytes */" + std::to_string(total_size));
        return response;
      }
      partial = true;
    }

    HttpResponse response;
    response.status_code = partial ? 206 : 200;
    response.reason = partial ? "Partial Content" : "OK";
    response.headers.emplace_back("Content-Type", guess_content_type(local_path));
    response.headers.emplace_back("Accept-Ranges", "bytes");
    response.headers.emplace_back("Content-Length", std::to_string(range_end - range_start + 1));
    response.headers.emplace_back("Connection", "close");
    if (partial) {
      response.headers.emplace_back(
          "Content-Range",
          "bytes " + std::to_string(range_start) + "-" + std::to_string(range_end) + "/" + std::to_string(total_size));
    }

    response.stream_body = [this,
                            provider = std::move(provider),
                            mailbox,
                            file_record = std::move(file_record),
                            offsets,
                            range_start,
                            range_end](const HttpChunkWriter& writer) {
      auto [block_index, offset_in_block] = find_block_for_offset(offsets, range_start);
      auto remaining = range_end - range_start + 1;
      const auto prefetch_session_id = start_prefetch_session(mailbox, file_record, block_index + 1);
      struct PrefetchGuard {
        HttpImapDownloadServer* server = nullptr;
        std::uint64_t session_id = 0;
        ~PrefetchGuard() {
          if (server != nullptr && session_id != 0) {
            server->stop_prefetch_session(session_id);
          }
        }
      } guard{this, prefetch_session_id};

      while (remaining > 0 && block_index < file_record.blocks.size()) {
        const auto block_data = load_block(provider, mailbox, file_record, file_record.blocks[block_index]);
        const auto available = block_data->payload.size() > offset_in_block
                                   ? block_data->payload.size() - static_cast<std::size_t>(offset_in_block)
                                   : 0;
        if (available == 0) {
          break;
        }

        const auto to_write = std::min<std::uint64_t>(remaining, available);
        const std::string_view chunk(reinterpret_cast<const char*>(block_data->payload.data()) +
                                         static_cast<std::ptrdiff_t>(offset_in_block),
                                     static_cast<std::size_t>(to_write));
        if (!writer(chunk)) {
          stop_prefetch_session(prefetch_session_id);
          guard.session_id = 0;
          infra::logging::log_info("http",
                                   "client disconnected, stop prefetch for " + mailbox + ":" + file_record.local_path);
          return false;
        }

        remaining -= to_write;
        advance_prefetch_session(prefetch_session_id);
        ++block_index;
        offset_in_block = 0;
      }
      return remaining == 0;
    };

    infra::logging::log_info("http",
                             "prepared stream " + mailbox + ":" + local_path + " bytes=" + std::to_string(range_start) +
                                 "-" + std::to_string(range_end));
    return response;
  } catch (const std::exception& ex) {
    infra::logging::log_error("http", ex.what());
    return make_text_response(500, std::string("internal error: ") + ex.what() + '\n');
  }
}

void HttpImapDownloadServer::handle_client(std::intptr_t client_socket) {
  SocketCloser client(static_cast<NativeSocket>(client_socket));

  std::string request_text;
  std::array<char, 4096> buffer{};
  while (request_text.find("\r\n\r\n") == std::string::npos && request_text.size() < kMaxRequestBytes) {
#ifdef _WIN32
    const auto received = recv(client.socket, buffer.data(), static_cast<int>(buffer.size()), 0);
#else
    const auto received = recv(client.socket, buffer.data(), buffer.size(), 0);
#endif
    if (received <= 0) {
      return;
    }
    request_text.append(buffer.data(), static_cast<std::size_t>(received));
  }

  HttpResponse response;
  try {
    response = handle_request(parse_http_request_text(request_text));
  } catch (const std::exception& ex) {
    response = make_text_response(400, std::string("bad request: ") + ex.what() + '\n');
  }

  std::ostringstream header_stream;
  header_stream << "HTTP/1.1 " << response.status_code << ' ' << response.reason << "\r\n";
  for (const auto& [key, value] : response.headers) {
    header_stream << key << ": " << value << "\r\n";
  }
  header_stream << "\r\n";
  if (!send_all(client.socket, header_stream.str())) {
    return;
  }

  if (response.stream_body) {
    response.stream_body([&](std::string_view chunk) {
      return send_all(client.socket, chunk);
    });
    return;
  }

  send_all(client.socket, response.body);
}

void HttpImapDownloadServer::serve() {
#ifdef _WIN32
  WSADATA wsa_data{};
  const auto wsa_ok = WSAStartup(MAKEWORD(2, 2), &wsa_data);
  if (wsa_ok != 0) {
    throw std::runtime_error("WSAStartup failed");
  }
  struct WsaCleanup {
    ~WsaCleanup() { WSACleanup(); }
  } cleanup;
#endif

  const auto [host, port] = split_host_port(config_.http_listen_addr);

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;

  addrinfo* addresses = nullptr;
  const auto host_arg = host.empty() ? nullptr : host.c_str();
  const auto gai_result = getaddrinfo(host_arg, port.c_str(), &hints, &addresses);
  if (gai_result != 0) {
#ifdef _WIN32
    throw std::runtime_error("getaddrinfo failed");
#else
    throw std::runtime_error(gai_strerror(gai_result));
#endif
  }

  NativeSocket listen_socket = kInvalidSocket;
  for (auto* addr = addresses; addr != nullptr; addr = addr->ai_next) {
    listen_socket = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
    if (listen_socket == kInvalidSocket) {
      continue;
    }

    int reuse = 1;
    setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    if (bind(listen_socket, addr->ai_addr, static_cast<int>(addr->ai_addrlen)) == 0 &&
        listen(listen_socket, SOMAXCONN) == 0) {
      break;
    }

    close_socket(listen_socket);
    listen_socket = kInvalidSocket;
  }
  freeaddrinfo(addresses);

  if (listen_socket == kInvalidSocket) {
    throw std::runtime_error("failed to bind HTTP server on " + config_.http_listen_addr);
  }

  SocketCloser listener(listen_socket);
  infra::logging::log_info("http", "HTTP-to-IMAP server listening on " + config_.http_listen_addr);
  for (;;) {
    sockaddr_storage client_addr{};
    socklen_t client_len = sizeof(client_addr);
    const auto client =
        accept(listener.socket, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
    if (client == kInvalidSocket) {
      continue;
    }

    std::thread([this, client]() {
      handle_client(static_cast<std::intptr_t>(client));
    }).detach();
  }
}

}  // namespace mailfs::application
