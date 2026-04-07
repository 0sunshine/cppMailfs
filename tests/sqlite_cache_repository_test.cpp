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

TEST(SQLiteCacheRepositoryTest, RemoveMessageUidClearsAffectedCacheEntries) {
  const auto db_path = std::filesystem::temp_directory_path() / "mailfs_repo_delete_test.db";
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
    metadata.subject = "movie/plain/2-2";
    metadata.block_seq = 2;
    repository.upsert_mail_block(101, metadata);

    repository.remove_message_uid("Archive", 100);

    EXPECT_TRUE(repository.get_cached_uids("Archive").empty());
    EXPECT_TRUE(repository.list_files("Archive").empty());
  }

  std::filesystem::remove(db_path);
}

TEST(SQLiteCacheRepositoryTest, PreservesLargeFileMetadataAndBlockSizes) {
  const auto db_path = std::filesystem::temp_directory_path() / "mailfs_repo_large_test.db";
  std::filesystem::remove(db_path);

  {
    mailfs::infra::storage::SQLiteCacheRepository repository(db_path);
    repository.initialize();

    constexpr std::uint64_t kLargeFileSize = 1073741824ULL;
    constexpr std::uint64_t kBlockSize = 134217728ULL;

    for (int seq = 1; seq <= 8; ++seq) {
      mailfs::core::model::MailBlockMetadata metadata;
      metadata.subject = "archive.iso/plain/" + std::to_string(seq) + "-8";
      metadata.file_md5 = "filemd5";
      metadata.block_md5 = "blockmd5-" + std::to_string(seq);
      metadata.file_size = kLargeFileSize;
      metadata.block_size = kBlockSize;
      metadata.create_time = "2026-04-01T00:00:00Z";
      metadata.owner = "owner";
      metadata.local_path = "/archive/archive.iso";
      metadata.mail_folder = "Archive";
      metadata.block_seq = seq;
      metadata.block_count = 8;
      repository.upsert_mail_block(1000 + seq, metadata);
    }

    const auto files = repository.list_files("Archive");
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files[0].file_size, kLargeFileSize);
    EXPECT_EQ(files[0].block_count, 8);
    ASSERT_EQ(files[0].blocks.size(), 8u);
    EXPECT_EQ(files[0].blocks.front().block_size, kBlockSize);
    EXPECT_EQ(files[0].blocks.back().uid, 1008u);
  }

  std::filesystem::remove(db_path);
}

TEST(SQLiteCacheRepositoryTest, ClearMailboxRemovesOnlyTargetMailboxEntries) {
  const auto db_path = std::filesystem::temp_directory_path() / "mailfs_repo_clear_mailbox_test.db";
  std::filesystem::remove(db_path);

  {
    mailfs::infra::storage::SQLiteCacheRepository repository(db_path);
    repository.initialize();

    mailfs::core::model::MailBlockMetadata archive_metadata;
    archive_metadata.subject = "movie/plain/1-1";
    archive_metadata.file_md5 = "archive-md5";
    archive_metadata.block_md5 = "archive-block";
    archive_metadata.file_size = 10;
    archive_metadata.block_size = 10;
    archive_metadata.create_time = "2026-04-01T00:00:00Z";
    archive_metadata.owner = "owner";
    archive_metadata.local_path = "/archive/movie.mp4";
    archive_metadata.mail_folder = "Archive";
    archive_metadata.block_seq = 1;
    archive_metadata.block_count = 1;
    repository.upsert_mail_block(100, archive_metadata);

    auto other_metadata = archive_metadata;
    other_metadata.file_md5 = "other-md5";
    other_metadata.block_md5 = "other-block";
    other_metadata.local_path = "/other/movie.mp4";
    other_metadata.mail_folder = "Other";
    repository.upsert_mail_block(200, other_metadata);

    repository.clear_mailbox("Archive");

    EXPECT_TRUE(repository.list_files("Archive").empty());
    EXPECT_TRUE(repository.get_cached_uids("Archive").empty());
    ASSERT_EQ(repository.list_files("Other").size(), 1u);
    EXPECT_TRUE(repository.get_cached_uids("Other").count(200));
  }

  std::filesystem::remove(db_path);
}

TEST(SQLiteCacheRepositoryTest, OpensDatabaseWithUtf8Path) {
  const auto db_path =
      std::filesystem::temp_directory_path() / std::filesystem::u8path(u8"\u7f13\u5b58_\u4ed3\u5e93\u6d4b\u8bd5.db");
  std::filesystem::remove(db_path);

  {
    mailfs::infra::storage::SQLiteCacheRepository repository(db_path);
    repository.initialize();
  }

  EXPECT_TRUE(std::filesystem::exists(db_path));
  std::filesystem::remove(db_path);
}
