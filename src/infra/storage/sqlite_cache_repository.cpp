#include "mailfs/infra/storage/sqlite_cache_repository.hpp"

#include <stdexcept>
#include <utility>

namespace mailfs::infra::storage {

namespace {

class Statement final {
 public:
  Statement(sqlite3* db, const char* sql) : db_(db) {
    if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
      throw std::runtime_error(sqlite3_errmsg(db_));
    }
  }

  ~Statement() {
    sqlite3_finalize(stmt_);
  }

  sqlite3_stmt* get() const { return stmt_; }

 private:
  sqlite3* db_ = nullptr;
  sqlite3_stmt* stmt_ = nullptr;
};

void bind_text(sqlite3_stmt* stmt, int index, const std::string& text) {
  if (sqlite3_bind_text(stmt, index, text.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK) {
    throw std::runtime_error("sqlite bind text failed");
  }
}

std::string column_text(sqlite3_stmt* stmt, int index) {
  const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, index));
  return text != nullptr ? text : "";
}

}  // namespace

SQLiteCacheRepository::SQLiteCacheRepository(std::filesystem::path database_path)
    : database_path_(std::move(database_path)) {
  if (sqlite3_open(database_path_.string().c_str(), &db_) != SQLITE_OK) {
    throw std::runtime_error("failed to open sqlite database");
  }
}

SQLiteCacheRepository::~SQLiteCacheRepository() {
  if (db_ != nullptr) {
    sqlite3_close(db_);
    db_ = nullptr;
  }
}

void SQLiteCacheRepository::initialize() {
  execute_sql("PRAGMA journal_mode=WAL;");
  execute_sql("CREATE TABLE IF NOT EXISTS cache_files ("
              "fileid INTEGER PRIMARY KEY AUTOINCREMENT,"
              "mailfolder TEXT NOT NULL,"
              "localpath TEXT NOT NULL,"
              "blockcount INTEGER NOT NULL,"
              "filemd5 TEXT NOT NULL,"
              "filesize INTEGER NOT NULL DEFAULT 0,"
              "UNIQUE(mailfolder, localpath)"
              ");");
  execute_sql("CREATE TABLE IF NOT EXISTS cache_blocks ("
              "fileid INTEGER NOT NULL,"
              "blockseq INTEGER NOT NULL,"
              "uid INTEGER NOT NULL,"
              "blockmd5 TEXT NOT NULL,"
              "blocksize INTEGER NOT NULL DEFAULT 0,"
              "UNIQUE(fileid, blockseq)"
              ");");
  execute_sql("CREATE INDEX IF NOT EXISTS idx_cache_blocks_uid ON cache_blocks(uid);");
  execute_sql("CREATE INDEX IF NOT EXISTS idx_cache_files_mailfolder ON cache_files(mailfolder);");
}

void SQLiteCacheRepository::upsert_mail_block(std::uint64_t uid, const core::model::MailBlockMetadata& metadata) {
  execute_sql("BEGIN IMMEDIATE TRANSACTION;");
  try {
    {
      Statement insert_file(db_,
                            "INSERT INTO cache_files (mailfolder, localpath, blockcount, filemd5, filesize) "
                            "VALUES (?, ?, ?, ?, ?) "
                            "ON CONFLICT(mailfolder, localpath) DO UPDATE SET "
                            "blockcount=excluded.blockcount, filemd5=excluded.filemd5, filesize=excluded.filesize;");
      bind_text(insert_file.get(), 1, metadata.mail_folder);
      bind_text(insert_file.get(), 2, metadata.local_path);
      sqlite3_bind_int(insert_file.get(), 3, metadata.block_count);
      bind_text(insert_file.get(), 4, metadata.file_md5);
      sqlite3_bind_int64(insert_file.get(), 5, static_cast<sqlite3_int64>(metadata.file_size));
      if (sqlite3_step(insert_file.get()) != SQLITE_DONE) {
        throw std::runtime_error(sqlite3_errmsg(db_));
      }
    }

    std::int64_t file_id = 0;
    {
      Statement select_file(db_, "SELECT fileid FROM cache_files WHERE mailfolder=? AND localpath=?;");
      bind_text(select_file.get(), 1, metadata.mail_folder);
      bind_text(select_file.get(), 2, metadata.local_path);
      if (sqlite3_step(select_file.get()) != SQLITE_ROW) {
        throw std::runtime_error("cache file record not found after upsert");
      }
      file_id = sqlite3_column_int64(select_file.get(), 0);
    }

    {
      Statement upsert_block(
          db_,
          "INSERT INTO cache_blocks (fileid, blockseq, uid, blockmd5, blocksize) "
          "VALUES (?, ?, ?, ?, ?) "
          "ON CONFLICT(fileid, blockseq) DO UPDATE SET "
          "uid=excluded.uid, blockmd5=excluded.blockmd5, blocksize=excluded.blocksize;");
      sqlite3_bind_int64(upsert_block.get(), 1, static_cast<sqlite3_int64>(file_id));
      sqlite3_bind_int(upsert_block.get(), 2, metadata.block_seq);
      sqlite3_bind_int64(upsert_block.get(), 3, static_cast<sqlite3_int64>(uid));
      bind_text(upsert_block.get(), 4, metadata.block_md5);
      sqlite3_bind_int64(upsert_block.get(), 5, static_cast<sqlite3_int64>(metadata.block_size));
      if (sqlite3_step(upsert_block.get()) != SQLITE_DONE) {
        throw std::runtime_error(sqlite3_errmsg(db_));
      }
    }

    execute_sql("COMMIT;");
  } catch (...) {
    execute_sql("ROLLBACK;");
    throw;
  }
}

std::set<std::uint64_t> SQLiteCacheRepository::get_cached_uids(const std::string& mailbox) const {
  Statement statement(
      db_,
      "SELECT b.uid FROM cache_blocks b INNER JOIN cache_files f ON b.fileid = f.fileid WHERE f.mailfolder = ?;");
  bind_text(statement.get(), 1, mailbox);

  std::set<std::uint64_t> uids;
  while (sqlite3_step(statement.get()) == SQLITE_ROW) {
    uids.insert(static_cast<std::uint64_t>(sqlite3_column_int64(statement.get(), 0)));
  }
  return uids;
}

std::vector<core::model::CachedFileRecord> SQLiteCacheRepository::list_files(const std::string& mailbox) const {
  Statement files_statement(
      db_,
      "SELECT fileid, mailfolder, localpath, blockcount, filemd5, filesize "
      "FROM cache_files WHERE mailfolder=? ORDER BY localpath;");
  bind_text(files_statement.get(), 1, mailbox);

  std::vector<core::model::CachedFileRecord> files;
  while (sqlite3_step(files_statement.get()) == SQLITE_ROW) {
    core::model::CachedFileRecord file;
    file.file_id = sqlite3_column_int64(files_statement.get(), 0);
    file.mail_folder = column_text(files_statement.get(), 1);
    file.local_path = column_text(files_statement.get(), 2);
    file.block_count = sqlite3_column_int(files_statement.get(), 3);
    file.file_md5 = column_text(files_statement.get(), 4);
    file.file_size = static_cast<std::uint64_t>(sqlite3_column_int64(files_statement.get(), 5));

    Statement block_statement(
        db_,
        "SELECT blockseq, uid, blockmd5, blocksize FROM cache_blocks WHERE fileid=? ORDER BY blockseq;");
    sqlite3_bind_int64(block_statement.get(), 1, static_cast<sqlite3_int64>(file.file_id));
    while (sqlite3_step(block_statement.get()) == SQLITE_ROW) {
      core::model::CachedBlockRecord block;
      block.block_seq = sqlite3_column_int(block_statement.get(), 0);
      block.uid = static_cast<std::uint64_t>(sqlite3_column_int64(block_statement.get(), 1));
      block.block_md5 = column_text(block_statement.get(), 2);
      block.block_size = static_cast<std::uint64_t>(sqlite3_column_int64(block_statement.get(), 3));
      file.blocks.push_back(std::move(block));
    }

    files.push_back(std::move(file));
  }

  return files;
}

std::optional<core::model::CachedFileRecord> SQLiteCacheRepository::find_file(const std::string& mailbox,
                                                                              const std::string& local_path) const {
  auto files = list_files(mailbox);
  for (auto& file : files) {
    if (file.local_path == local_path) {
      return file;
    }
  }
  return std::nullopt;
}

void SQLiteCacheRepository::execute_sql(const char* sql) const {
  char* message = nullptr;
  if (sqlite3_exec(db_, sql, nullptr, nullptr, &message) != SQLITE_OK) {
    std::string error = message != nullptr ? message : "unknown sqlite error";
    sqlite3_free(message);
    throw std::runtime_error(error);
  }
}

}  // namespace mailfs::infra::storage
