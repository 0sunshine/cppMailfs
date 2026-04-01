#pragma once

#include <filesystem>

#include <sqlite3.h>

#include "mailfs/application/ports/cache_repository.hpp"

namespace mailfs::infra::storage {

class SQLiteCacheRepository final : public application::ports::ICacheRepository {
 public:
  explicit SQLiteCacheRepository(std::filesystem::path database_path);
  ~SQLiteCacheRepository() override;

  SQLiteCacheRepository(const SQLiteCacheRepository&) = delete;
  SQLiteCacheRepository& operator=(const SQLiteCacheRepository&) = delete;

  void initialize() override;
  void upsert_mail_block(std::uint64_t uid, const core::model::MailBlockMetadata& metadata) override;
  std::set<std::uint64_t> get_cached_uids(const std::string& mailbox) const override;
  std::vector<core::model::CachedFileRecord> list_files(const std::string& mailbox) const override;
  std::optional<core::model::CachedFileRecord> find_file(const std::string& mailbox,
                                                         const std::string& local_path) const override;

 private:
  std::filesystem::path database_path_;
  sqlite3* db_ = nullptr;

  void execute_sql(const char* sql) const;
};

}  // namespace mailfs::infra::storage
