#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#include <gtest/gtest.h>

#include "mailfs/application/mailfs_service.hpp"
#include "mailfs/infra/logging/logger.hpp"

namespace {

class TestTransport final : public mailfs::application::ports::IMailTransport {
 public:
  void connect(const mailfs::core::model::AppConfig&,
               const std::string& username,
               const std::string& password) override {
    connected_username = username;
    connected_password = password;
  }

  void disconnect() noexcept override {}

  std::vector<std::string> list_mailboxes(const std::string&) override {
    return {u8"\u6536\u4ef6\u7bb1"};
  }

  void select_mailbox(const std::string& mailbox) override {
    selected_mailboxes.push_back(mailbox);
  }

  std::vector<std::uint64_t> search_all_uids() override {
    return search_result;
  }

  std::vector<mailfs::application::ports::FetchedMetadata> fetch_metadata(
      const std::vector<std::uint64_t>& uids) override {
    fetch_batches.push_back(uids);
    std::vector<mailfs::application::ports::FetchedMetadata> result;
    for (const auto uid : uids) {
      const auto it = metadata_by_uid.find(uid);
      if (it != metadata_by_uid.end()) {
        result.push_back({uid, it->second});
      }
    }
    return result;
  }

  std::vector<mailfs::application::ports::FetchedMessage> fetch_messages(
      const std::vector<std::uint64_t>& uids) override {
    std::vector<mailfs::application::ports::FetchedMessage> result;
    for (const auto uid : uids) {
      const auto it = messages_by_uid.find(uid);
      if (it != messages_by_uid.end()) {
        result.push_back({uid, it->second});
      }
    }
    return result;
  }

  void delete_message_by_uid(std::uint64_t) override {}

  void append_message(const std::string&, const std::string&) override {}

  std::string connected_username;
  std::string connected_password;
  std::vector<std::string> selected_mailboxes;
  std::vector<std::uint64_t> search_result;
  std::map<std::uint64_t, std::string> metadata_by_uid;
  std::map<std::uint64_t, std::string> messages_by_uid;
  std::vector<std::vector<std::uint64_t>> fetch_batches;
};

class TestRepository final : public mailfs::application::ports::ICacheRepository {
 public:
  void initialize() override {
    initialized = true;
  }

  void upsert_mail_block(std::uint64_t uid, const mailfs::core::model::MailBlockMetadata& metadata) override {
    upserts.emplace_back(uid, metadata);
  }

  void remove_message_uid(const std::string&, std::uint64_t) override {}

  std::set<std::uint64_t> get_cached_uids(const std::string&) const override {
    return {};
  }

  std::vector<mailfs::core::model::CachedFileRecord> list_files(const std::string&) const override {
    return {};
  }

  std::optional<mailfs::core::model::CachedFileRecord> find_file(const std::string&,
                                                                 const std::string&) const override {
    return std::nullopt;
  }

  bool initialized = false;
  std::vector<std::pair<std::uint64_t, mailfs::core::model::MailBlockMetadata>> upserts;
};

}  // namespace

TEST(MailfsServiceTest, ReadsUtf8CredentialPathAndKeepsUtf8MailboxNames) {
  const auto credential_path =
      std::filesystem::temp_directory_path() / std::filesystem::u8path(u8"mailfs_\u51ed\u636e.txt");
  std::ofstream output(credential_path, std::ios::binary);
  ASSERT_TRUE(output.is_open());
  output << "user@example.com\n";
  output << "s3cret\n";
  output.close();

  mailfs::core::model::AppConfig config;
  config.credential_file = credential_path.u8string();

  TestTransport transport;
  TestRepository repository;
  mailfs::application::MailfsService service(config, transport, repository);

  const auto mailboxes = service.list_mailboxes();

  EXPECT_TRUE(repository.initialized);
  EXPECT_EQ(transport.connected_username, "user@example.com");
  EXPECT_EQ(transport.connected_password, "s3cret");
  ASSERT_EQ(mailboxes.size(), 1u);
  EXPECT_EQ(mailboxes.front(), u8"\u6536\u4ef6\u7bb1");

  std::filesystem::remove(credential_path);
}

TEST(MailfsServiceTest, CachesMailboxInBatchesAndParsesLegacyMetadata) {
  const auto mailbox = std::string(
      "\xE5\x85\xB6\xE4\xBB\x96\xE6\x96\x87\xE4\xBB\xB6\xE5\xA4\xB9\x2F\xE4\xB8\xAA\xE4\xBA\xBA\x70\x63");
  const auto credential_path = std::filesystem::temp_directory_path() / "mailfs_cache_batches.txt";
  std::ofstream output(credential_path, std::ios::binary);
  ASSERT_TRUE(output.is_open());
  output << "user@example.com\n";
  output << "s3cret\n";
  output.close();

  mailfs::core::model::AppConfig config;
  config.credential_file = credential_path.u8string();
  config.cache_fetch_batch_size = 2;
  mailfs::infra::logging::Logger::instance().reset_for_tests();
  mailfs::infra::logging::Logger::instance().configure({
      mailfs::infra::logging::LogLevel::kOff,
      {},
      false,
  });

  TestTransport transport;
  transport.search_result = {101, 102, 103, 104, 105};
  const std::string legacy_metadata =
      "subject:demo.bin/plain/1-1\r\n"
      "filemd5:file-1\r\n"
      "blockmd5:block-1\r\n"
      "filesize:10\r\n"
      "blocksize:10\r\n"
      "createtime:2026-04-01T00:00:00Z\r\n"
      "owner:user@example.com\r\n"
      "localpath:/demo/demo.bin\r\n"
      "mailfolder:" + mailbox + "\r\n";
  for (const auto uid : transport.search_result) {
    transport.metadata_by_uid.emplace(uid, legacy_metadata);
  }

  TestRepository repository;
  mailfs::application::MailfsService service(config, transport, repository);

  const auto fetched = service.cache_mailbox(mailbox);

  EXPECT_EQ(fetched, 5u);
  ASSERT_EQ(transport.fetch_batches.size(), 3u);
  EXPECT_EQ(transport.fetch_batches[0], (std::vector<std::uint64_t>{101, 102}));
  EXPECT_EQ(transport.fetch_batches[1], (std::vector<std::uint64_t>{103, 104}));
  EXPECT_EQ(transport.fetch_batches[2], (std::vector<std::uint64_t>{105}));
  ASSERT_EQ(repository.upserts.size(), 5u);
  EXPECT_EQ(repository.upserts.front().second.local_path, "/demo/demo.bin");
  EXPECT_EQ(repository.upserts.front().second.mail_folder, mailbox);

  mailfs::infra::logging::Logger::instance().reset_for_tests();
  std::filesystem::remove(credential_path);
}

TEST(MailfsServiceTest, DecodesQuotedPrintableMetadataBeforeCaching) {
  const auto mailbox = std::string(
      "\xE5\x85\xB6\xE4\xBB\x96\xE6\x96\x87\xE4\xBB\xB6\xE5\xA4\xB9\x2F\xE4\xB8\xAA\xE4\xBA\xBA\x70\x63");
  const auto credential_path = std::filesystem::temp_directory_path() / "mailfs_qp_batches.txt";
  std::ofstream output(credential_path, std::ios::binary);
  ASSERT_TRUE(output.is_open());
  output << "user@example.com\n";
  output << "s3cret\n";
  output.close();

  mailfs::core::model::AppConfig config;
  config.credential_file = credential_path.u8string();
  config.cache_fetch_batch_size = 8;

  TestTransport transport;
  transport.search_result = {2001};
  transport.metadata_by_uid.emplace(
      2001,
      "subject:demo.mp4/plain/1-7\r\n"
      "filemd5:file-1\r\n"
      "blockmd5:block-1\r\n"
      "filesize:212241312\r\n"
      "blocksize:33554432\r\n"
      "createtime:2026-03-08T23:55:47+08:00\r\n"
      "owner:sunshine\r\n"
      "localpath:G:/BaiduNetdiskDownload/00.=E9=A9=AC=E5=93=A5go=E4=B8=83=E6=9C=9F=/demo.mp4\r\n"
      "mailfolder:=E5=85=B6=E4=BB=96=E6=96=87=E4=BB=B6=E5=A4=B9/=E4=B8=AA=E4=BA=BApc\r\n");

  TestRepository repository;
  mailfs::application::MailfsService service(config, transport, repository);

  const auto fetched = service.cache_mailbox(mailbox);

  EXPECT_EQ(fetched, 1u);
  ASSERT_EQ(repository.upserts.size(), 1u);
  EXPECT_EQ(repository.upserts.front().second.mail_folder, mailbox);
  EXPECT_EQ(repository.upserts.front().second.local_path,
            std::string("G:/BaiduNetdiskDownload/00.") +
                "\xE9\xA9\xAC\xE5\x93\xA5\x67\x6F\xE4\xB8\x83\xE6\x9C\x9F" + "/demo.mp4");

  std::filesystem::remove(credential_path);
}
