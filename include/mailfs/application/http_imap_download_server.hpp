#pragma once

#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <thread>

#include "mailfs/application/mailfs_service.hpp"
#include "mailfs/core/model/app_config.hpp"
#include "mailfs/core/model/cache_models.hpp"

namespace mailfs::application {

struct HttpRequest {
  std::string method;
  std::string target;
  std::unordered_map<std::string, std::string> headers;
};

using HttpChunkWriter = std::function<bool(std::string_view chunk)>;

struct HttpResponse {
  int status_code = 200;
  std::string reason = "OK";
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  std::function<bool(const HttpChunkWriter& writer)> stream_body;
};

class IHttpImapDownloadProvider {
 public:
  virtual ~IHttpImapDownloadProvider() = default;

  virtual core::model::CachedFileRecord resolve_file(const std::string& mailbox, const std::string& local_path) = 0;
  virtual DownloadedBlockData fetch_block(const std::string& mailbox,
                                          const core::model::CachedFileRecord& file_record,
                                          const core::model::CachedBlockRecord& block) = 0;
};

class HttpImapDownloadServer {
 public:
  using DownloadProviderFactory = std::function<std::shared_ptr<IHttpImapDownloadProvider>()>;

  explicit HttpImapDownloadServer(core::model::AppConfig config);
  HttpImapDownloadServer(core::model::AppConfig config, DownloadProviderFactory provider_factory);
  ~HttpImapDownloadServer();

  HttpResponse handle_request(const HttpRequest& request);
  void serve();
  void stop();

 private:
  core::model::AppConfig config_;
  DownloadProviderFactory provider_factory_;

  struct BlockCacheEntry {
    std::shared_ptr<DownloadedBlockData> data;
    std::list<std::string>::iterator order;
  };

  struct BlockLoadState {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    std::shared_ptr<DownloadedBlockData> data;
    std::string error;
  };

  struct PrefetchTask {
    std::uint64_t session_id = 0;
    std::string cache_key;
    std::string mailbox;
    core::model::CachedFileRecord file_record;
    std::size_t block_index = 0;
  };

  struct PrefetchSession {
    std::string mailbox;
    core::model::CachedFileRecord file_record;
    std::size_t next_block_index = 0;
    std::size_t allowed_block_index = 0;
    bool task_inflight_or_queued = false;
    bool active = true;
  };

  std::shared_ptr<DownloadedBlockData> load_block_impl(const std::shared_ptr<IHttpImapDownloadProvider>& provider,
                                                       const std::string& cache_key,
                                                       const std::string& mailbox,
                                                       const core::model::CachedFileRecord& file_record,
                                                       const core::model::CachedBlockRecord& block);
  std::string make_block_cache_key(const std::string& mailbox, std::uint64_t uid) const;
  bool has_cached_block(const std::string& cache_key) const;
  std::uint64_t start_prefetch_session(const std::string& mailbox,
                                       const core::model::CachedFileRecord& file_record,
                                       std::size_t first_block_index);
  void stop_prefetch_session(std::uint64_t session_id);
  void advance_prefetch_session(std::uint64_t session_id);
  void enqueue_next_prefetch_locked(std::uint64_t session_id);
  void prefetch_worker_loop();

  std::shared_ptr<DownloadedBlockData> load_block(const std::shared_ptr<IHttpImapDownloadProvider>& provider,
                                                  const std::string& mailbox,
                                                  const core::model::CachedFileRecord& file_record,
                                                  const core::model::CachedBlockRecord& block);
  HttpResponse make_text_response(int status_code, std::string body) const;
  void handle_client(std::intptr_t client_socket);

  mutable std::mutex block_cache_mutex_;
  std::list<std::string> block_cache_order_;
  std::unordered_map<std::string, BlockCacheEntry> block_cache_;
  std::unordered_map<std::string, std::shared_ptr<BlockLoadState>> block_loads_;

  std::mutex prefetch_mutex_;
  std::condition_variable prefetch_cv_;
  std::deque<PrefetchTask> prefetch_queue_;
  std::unordered_map<std::uint64_t, PrefetchSession> prefetch_sessions_;
  std::unordered_set<std::string> prefetch_pending_keys_;
  std::thread prefetch_worker_;
  bool stop_prefetch_worker_ = false;
  std::uint64_t next_prefetch_session_id_ = 1;
  std::atomic_bool stop_serving_{false};
};

}  // namespace mailfs::application
