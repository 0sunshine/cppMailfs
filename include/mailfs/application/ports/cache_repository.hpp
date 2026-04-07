#pragma once

#include <optional>
#include <set>
#include <string>
#include <vector>

#include "mailfs/core/model/cache_models.hpp"
#include "mailfs/core/model/mail_block_metadata.hpp"

namespace mailfs::application::ports {

class ICacheRepository {
 public:
  virtual ~ICacheRepository() = default;

  virtual void initialize() = 0;
  virtual void upsert_mail_block(std::uint64_t uid, const core::model::MailBlockMetadata& metadata) = 0;
  virtual void remove_message_uid(const std::string& mailbox, std::uint64_t uid) = 0;
  virtual void clear_mailbox(const std::string& mailbox) = 0;
  virtual std::set<std::uint64_t> get_cached_uids(const std::string& mailbox) const = 0;
  virtual std::vector<core::model::CachedFileRecord> list_files(const std::string& mailbox) const = 0;
  virtual std::optional<core::model::CachedFileRecord> find_file(const std::string& mailbox,
                                                                 const std::string& local_path) const = 0;
};

}  // namespace mailfs::application::ports
