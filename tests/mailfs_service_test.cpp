#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

#include "mailfs/application/mailfs_service.hpp"

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

  void select_mailbox(const std::string&) override {}

  std::vector<std::uint64_t> search_all_uids() override {
    return {};
  }

  std::vector<mailfs::application::ports::FetchedMessage> fetch_messages(
      const std::vector<std::uint64_t>&) override {
    return {};
  }

  void delete_message_by_uid(std::uint64_t) override {}

  void append_message(const std::string&, const std::string&) override {}

  std::string connected_username;
  std::string connected_password;
};

class TestRepository final : public mailfs::application::ports::ICacheRepository {
 public:
  void initialize() override {
    initialized = true;
  }

  void upsert_mail_block(std::uint64_t, const mailfs::core::model::MailBlockMetadata&) override {}

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
