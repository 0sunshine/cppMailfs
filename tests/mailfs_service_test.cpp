#include <filesystem>
#include <fstream>
#include <tuple>
#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "mailfs/application/mailfs_service.hpp"
#include "mailfs/core/hash.hpp"
#include "mailfs/core/mime/mime_message.hpp"
#include "mailfs/core/model/mail_block_metadata.hpp"
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

  void append_message(const std::string& mailbox, const std::string& raw_message) override {
    appended_mailboxes.push_back(mailbox);
    appended_messages.push_back(raw_message);
  }

  std::string connected_username;
  std::string connected_password;
  std::vector<std::string> selected_mailboxes;
  std::vector<std::uint64_t> search_result;
  std::map<std::uint64_t, std::string> metadata_by_uid;
  std::map<std::uint64_t, std::string> messages_by_uid;
  std::vector<std::vector<std::uint64_t>> fetch_batches;
  std::vector<std::string> appended_mailboxes;
  std::vector<std::string> appended_messages;
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

class DownloadRepository final : public mailfs::application::ports::ICacheRepository {
 public:
  void initialize() override {}

  void upsert_mail_block(std::uint64_t, const mailfs::core::model::MailBlockMetadata&) override {}

  void remove_message_uid(const std::string&, std::uint64_t) override {}

  std::set<std::uint64_t> get_cached_uids(const std::string&) const override {
    return {};
  }

  std::vector<mailfs::core::model::CachedFileRecord> list_files(const std::string& mailbox) const override {
    if (file.mail_folder == mailbox) {
      return {file};
    }
    return {};
  }

  std::optional<mailfs::core::model::CachedFileRecord> find_file(const std::string& mailbox,
                                                                 const std::string& local_path) const override {
    if (file.mail_folder == mailbox && file.local_path == local_path) {
      return file;
    }
    return std::nullopt;
  }

  mailfs::core::model::CachedFileRecord file;
};

std::string build_test_mail_message(const mailfs::core::model::MailBlockMetadata& metadata,
                                    const std::string& display_name,
                                    const std::string& email,
                                    const std::string& attachment_name,
                                    const std::vector<unsigned char>& payload) {
  mailfs::core::mime::MimeMessage message;
  message.headers["From"] = "\"" + display_name + "\" <" + email + ">";
  message.headers["To"] = "\"" + display_name + "\" <" + email + ">";
  message.headers["Subject"] = metadata.subject;

  mailfs::core::mime::MimePart metadata_part;
  metadata_part.headers["Content-Type"] = "text/plain; charset=utf-8";
  metadata_part.headers["Content-Transfer-Encoding"] = "quoted-printable";
  const auto metadata_text = metadata.to_legacy_text();
  metadata_part.body.assign(metadata_text.begin(), metadata_text.end());

  mailfs::core::mime::MimePart file_part;
  file_part.headers["Content-Type"] = "text/plain";
  file_part.headers["Content-Transfer-Encoding"] = "base64";
  file_part.headers["Content-Disposition"] = "attachment; filename=\"" + attachment_name + "\"";
  file_part.body = payload;

  message.parts.push_back(std::move(metadata_part));
  message.parts.push_back(std::move(file_part));
  return message.render_multipart_mixed("test-boundary");
}

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
  mailfs::infra::logging::Logger::instance().reset_for_tests();
  mailfs::infra::logging::Logger::instance().configure({
      mailfs::infra::logging::LogLevel::kOff,
      {},
      false,
  });

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
  mailfs::infra::logging::Logger::instance().reset_for_tests();
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

  std::vector<std::pair<std::size_t, std::size_t>> progress_events;
  const auto fetched = service.cache_mailbox(mailbox, [&](std::size_t done, std::size_t total) {
    progress_events.emplace_back(done, total);
  });

  EXPECT_EQ(fetched, 5u);
  ASSERT_FALSE(progress_events.empty());
  EXPECT_EQ(progress_events.front(), (std::pair<std::size_t, std::size_t>{0u, 5u}));
  EXPECT_EQ(progress_events.back(), (std::pair<std::size_t, std::size_t>{5u, 5u}));
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
  mailfs::infra::logging::Logger::instance().reset_for_tests();
  mailfs::infra::logging::Logger::instance().configure({
      mailfs::infra::logging::LogLevel::kOff,
      {},
      false,
  });

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
  EXPECT_NE(repository.upserts.front().second.local_path.find(
                std::string("G:/BaiduNetdiskDownload/00.") +
                "\xE9\xA9\xAC\xE5\x93\xA5\x67\x6F\xE4\xB8\x83\xE6\x9C\x9F"),
            std::string::npos);
  EXPECT_EQ(repository.upserts.front().second.local_path.substr(
                repository.upserts.front().second.local_path.size() - std::string("=/demo.mp4").size()),
            "=/demo.mp4");

  std::filesystem::remove(credential_path);
  mailfs::infra::logging::Logger::instance().reset_for_tests();
}

TEST(MailfsServiceTest, DecodesBase64JsonMetadataBeforeCaching) {
  const auto mailbox = std::string(
      "\xE5\x85\xB6\xE4\xBB\x96\xE6\x96\x87\xE4\xBB\xB6\xE5\xA4\xB9\x2F\x63\x6F\x64\x65\x78\xE6\xB5\x8B\xE8\xAF\x95");
  const auto credential_path = std::filesystem::temp_directory_path() / "mailfs_b64_batches.txt";
  std::ofstream output(credential_path, std::ios::binary);
  ASSERT_TRUE(output.is_open());
  output << "user@example.com\n";
  output << "s3cret\n";
  output.close();

  mailfs::core::model::AppConfig config;
  config.credential_file = credential_path.u8string();
  config.cache_fetch_batch_size = 8;
  mailfs::infra::logging::Logger::instance().reset_for_tests();
  mailfs::infra::logging::Logger::instance().configure({
      mailfs::infra::logging::LogLevel::kOff,
      {},
      false,
  });

  TestTransport transport;
  transport.search_result = {3001};
  transport.metadata_by_uid.emplace(
      3001,
      "eyJibG9ja19jb3VudCI6MSwiYmxvY2tfbWQ1IjoiYmxvY2stMSIsImJsb2NrX3NlcSI6MSwiYmxvY2tfc2l6ZSI6MTAsImNyZWF0ZV90aW1lIjoi"
      "MjAyNi0wNC0wMVQxMjo1NDowNVoiLCJlbmNyeXB0ZWQiOmZhbHNlLCJmaWxlX21kNSI6ImZpbGUtMSIsImZpbGVfc2l6ZSI6MTAsImxvY2FsX3Bh"
      "dGgiOiJjb2RleC1jb21wYXJlLzIwMjYwNDAxL3NhbXBsZS5tcDQiLCJtYWlsX2ZvbGRlciI6IuWFtuS7luaWh+S7tuWkuS9jb2RleOa1i+ivlSIs"
      "Im93bmVyIjoidXNlckBleGFtcGxlLmNvbSIsInN1YmplY3QiOiJzYW1wbGUubXA0L3BsYWluLzEtMSJ9");

  TestRepository repository;
  mailfs::application::MailfsService service(config, transport, repository);

  const auto fetched = service.cache_mailbox(mailbox);

  EXPECT_EQ(fetched, 1u);
  ASSERT_EQ(repository.upserts.size(), 1u);
  EXPECT_EQ(repository.upserts.front().second.mail_folder, mailbox);
  EXPECT_EQ(repository.upserts.front().second.local_path, "codex-compare/20260401/sample.mp4");
  EXPECT_EQ(repository.upserts.front().second.file_md5, "file-1");

  std::filesystem::remove(credential_path);
  mailfs::infra::logging::Logger::instance().reset_for_tests();
}

TEST(MailfsServiceTest, UploadUsesConfiguredOwnerAndAbsoluteLocalPathInMetadata) {
  const auto credential_path = std::filesystem::temp_directory_path() / "mailfs_upload_owner.txt";
  {
    std::ofstream output(credential_path, std::ios::binary);
    ASSERT_TRUE(output.is_open());
    output << "user@example.com\n";
    output << "s3cret\n";
  }

  const auto upload_dir = std::filesystem::temp_directory_path() / "mailfs_upload_dir";
  std::filesystem::create_directories(upload_dir);
  const auto local_file = upload_dir / std::filesystem::u8path(u8"示例.txt");
  {
    std::ofstream output(local_file, std::ios::binary);
    ASSERT_TRUE(output.is_open());
    output << "hello from upload test";
  }

  mailfs::core::model::AppConfig config;
  config.credential_file = credential_path.u8string();
  config.owner_name = "sunshine";
  config.default_block_size = 1024;
  mailfs::infra::logging::Logger::instance().reset_for_tests();
  mailfs::infra::logging::Logger::instance().configure({
      mailfs::infra::logging::LogLevel::kOff,
      {},
      false,
  });

  TestTransport transport;
  TestRepository repository;
  mailfs::application::MailfsService service(config, transport, repository);

  std::vector<std::tuple<std::int64_t, std::int64_t, std::string>> progress_events;
  service.upload_file(u8"其他文件夹/codex测试", local_file, [&](std::int64_t current,
                                                              std::int64_t total,
                                                              const std::string& file_name) {
    progress_events.emplace_back(current, total, file_name);
  });

  ASSERT_EQ(transport.appended_mailboxes.size(), 1u);
  EXPECT_EQ(transport.appended_mailboxes.front(), u8"其他文件夹/codex测试");
  ASSERT_EQ(transport.appended_messages.size(), 1u);
  ASSERT_EQ(progress_events.size(), 1u);
  EXPECT_EQ(progress_events.front(), (std::tuple<std::int64_t, std::int64_t, std::string>{1, 1, local_file.filename().u8string()}));

  const auto parsed = mailfs::core::mime::MimeMessage::parse(transport.appended_messages.front());
  ASSERT_EQ(parsed.parts.size(), 2u);
  const std::string metadata_text(parsed.parts[0].body.begin(), parsed.parts[0].body.end());
  const auto metadata = mailfs::core::model::MailBlockMetadata::from_serialized_text(metadata_text);

  EXPECT_EQ(metadata.owner, "sunshine");
  EXPECT_EQ(metadata.local_path, std::filesystem::absolute(local_file).lexically_normal().u8string());
  EXPECT_EQ(metadata.subject, std::string(local_file.filename().u8string()) + "/plain/1-1");
  EXPECT_EQ(parsed.parts[1].headers.at("Content-Disposition"),
            "attachment; filename=\"" + local_file.filename().u8string() + "\"");

  std::filesystem::remove(local_file);
  std::filesystem::remove(upload_dir);
  std::filesystem::remove(credential_path);
  mailfs::infra::logging::Logger::instance().reset_for_tests();
}

TEST(MailfsServiceTest, ListCacheAndDownloadUseProgressCallbacksAndDerivedSavePath) {
  const auto credential_path = std::filesystem::temp_directory_path() / "mailfs_download_owner.txt";
  {
    std::ofstream output(credential_path, std::ios::binary);
    ASSERT_TRUE(output.is_open());
    output << "user@example.com\n";
    output << "s3cret\n";
  }

  const auto download_root = std::filesystem::temp_directory_path() / "mailfs_download_root";
  std::filesystem::create_directories(download_root);

  const std::string mailbox = u8"其他文件夹/codex测试";
  const std::string local_path = "G:/dataset/demo.bin";
  const std::vector<unsigned char> block1 = {'a', 'b', 'c'};
  const std::vector<unsigned char> block2 = {'d', 'e', 'f'};
  const std::vector<unsigned char> file_bytes = {'a', 'b', 'c', 'd', 'e', 'f'};

  mailfs::core::model::CachedFileRecord file;
  file.file_id = 1;
  file.mail_folder = mailbox;
  file.local_path = local_path;
  file.block_count = 2;
  file.file_md5 = mailfs::core::md5_hex(file_bytes);
  file.file_size = file_bytes.size();
  file.blocks = {
      {1, 11, mailfs::core::md5_hex(block1), block1.size()},
      {2, 12, mailfs::core::md5_hex(block2), block2.size()},
  };

  mailfs::core::model::MailBlockMetadata meta1;
  meta1.subject = "demo.bin/plain/1-2";
  meta1.file_md5 = file.file_md5;
  meta1.block_md5 = file.blocks[0].block_md5;
  meta1.file_size = file.file_size;
  meta1.block_size = file.blocks[0].block_size;
  meta1.create_time = "2026-04-01T00:00:00Z";
  meta1.owner = "sunshine";
  meta1.local_path = local_path;
  meta1.mail_folder = mailbox;
  meta1.block_seq = 1;
  meta1.block_count = 2;

  auto meta2 = meta1;
  meta2.subject = "demo.bin/plain/2-2";
  meta2.block_md5 = file.blocks[1].block_md5;
  meta2.block_size = file.blocks[1].block_size;
  meta2.block_seq = 2;

  TestTransport transport;
  transport.messages_by_uid.emplace(11, build_test_mail_message(meta1, "mailfs", "user@example.com", "demo.bin", block1));
  transport.messages_by_uid.emplace(12, build_test_mail_message(meta2, "mailfs", "user@example.com", "demo.bin", block2));

  DownloadRepository repository;
  repository.file = file;

  mailfs::core::model::AppConfig config;
  config.credential_file = credential_path.u8string();
  config.download_dir = download_root.u8string();
  mailfs::infra::logging::Logger::instance().reset_for_tests();
  mailfs::infra::logging::Logger::instance().configure({
      mailfs::infra::logging::LogLevel::kOff,
      {},
      false,
  });

  mailfs::application::MailfsService service(config, transport, repository);

  std::vector<std::tuple<std::size_t, std::size_t, std::string>> list_progress;
  const auto files = service.list_cached_files(mailbox, [&](std::size_t done, std::size_t total, const std::string& path) {
    list_progress.emplace_back(done, total, path);
  });
  ASSERT_EQ(files.size(), 1u);
  ASSERT_EQ(list_progress.size(), 1u);
  EXPECT_EQ(std::get<0>(list_progress.front()), 1u);
  EXPECT_EQ(std::get<1>(list_progress.front()), 1u);
  EXPECT_EQ(std::get<2>(list_progress.front()), local_path);

  std::vector<std::tuple<std::int64_t, std::int64_t, std::string>> download_progress;
  const auto saved_path = service.download_file(mailbox, local_path, [&](std::int64_t current,
                                                                        std::int64_t total,
                                                                        const std::string& file_name) {
    download_progress.emplace_back(current, total, file_name);
  });

  EXPECT_EQ(saved_path, download_root / "G" / "dataset" / "demo.bin");
  ASSERT_EQ(download_progress.size(), 2u);
  EXPECT_EQ(download_progress.front(), (std::tuple<std::int64_t, std::int64_t, std::string>{1, 2, "demo.bin"}));
  EXPECT_EQ(download_progress.back(), (std::tuple<std::int64_t, std::int64_t, std::string>{2, 2, "demo.bin"}));
  EXPECT_TRUE(std::filesystem::exists(saved_path));
  EXPECT_EQ(mailfs::core::md5_hex(saved_path), file.file_md5);

  std::filesystem::remove(saved_path);
  std::filesystem::remove_all(saved_path.parent_path().parent_path());
  std::filesystem::remove(credential_path);
  mailfs::infra::logging::Logger::instance().reset_for_tests();
}
