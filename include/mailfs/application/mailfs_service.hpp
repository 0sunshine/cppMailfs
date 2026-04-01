#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "mailfs/application/ports/cache_repository.hpp"
#include "mailfs/application/ports/mail_transport.hpp"
#include "mailfs/core/model/app_config.hpp"
#include "mailfs/core/model/cache_models.hpp"

namespace mailfs::application {

class MailfsService {
 public:
  MailfsService(core::model::AppConfig config,
                ports::IMailTransport& transport,
                ports::ICacheRepository& repository);

  void connect();
  void disconnect() noexcept;
  std::vector<std::string> list_mailboxes();
  std::size_t cache_mailbox(const std::string& mailbox);
  std::vector<core::model::CachedFileRecord> list_cached_files(const std::string& mailbox);
  void delete_message_uid(const std::string& mailbox, std::uint64_t uid);
  void upload_file(const std::string& mailbox,
                   const std::filesystem::path& local_file,
                   const std::string& remote_path);
  void download_file(const std::string& mailbox,
                     const std::string& remote_path,
                     const std::filesystem::path& output_file);

 private:
  core::model::AppConfig config_;
  ports::IMailTransport& transport_;
  ports::ICacheRepository& repository_;
  std::string username_;
  std::string password_;
  bool connected_ = false;

  void ensure_connected();
  void ensure_mailbox_selected(const std::string& mailbox);
  std::pair<std::string, std::string> load_credentials() const;
};

}  // namespace mailfs::application
