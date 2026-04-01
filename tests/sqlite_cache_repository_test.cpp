#include <filesystem>

#include <gtest/gtest.h>

#include "mailfs/infra/storage/sqlite_cache_repository.hpp"

TEST(SQLiteCacheRepositoryTest, UpsertsAndQueriesBlocks) {
  const auto db_path = std::filesystem::temp_directory_path() / "mailfs_repo_test.db";
  std::filesystem::remove(db_path);

  {
    mailfs::infra::storage::SQLiteCacheRepository repository(db_path);
    repository.initialize();

    mailfs::core::model::MailBlockMetadata metadata;
    metadata.subject = "movie/plain/1-2";
    metadata.file_md5 = "filemd5";
    metadata.block_md5 = "blockmd5";
    metadata.file_size = 20;
    metadata.block_size = 10;
    metadata.create_time = "2026-04-01T00:00:00Z";
    metadata.owner = "owner";
    metadata.local_path = "/movie";
    metadata.mail_folder = "Archive";
    metadata.block_seq = 1;
    metadata.block_count = 2;

    repository.upsert_mail_block(100, metadata);

    const auto uids = repository.get_cached_uids("Archive");
    ASSERT_EQ(uids.size(), 1u);
    EXPECT_TRUE(uids.count(100));

    const auto files = repository.list_files("Archive");
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files[0].local_path, "/movie");
    ASSERT_EQ(files[0].blocks.size(), 1u);
    EXPECT_EQ(files[0].blocks[0].uid, 100u);
  }

  std::filesystem::remove(db_path);
}
