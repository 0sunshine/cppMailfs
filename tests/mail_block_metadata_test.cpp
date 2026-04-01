#include <gtest/gtest.h>

#include "mailfs/core/model/mail_block_metadata.hpp"

TEST(MailBlockMetadataTest, JsonRoundTripPreservesFields) {
  mailfs::core::model::MailBlockMetadata metadata;
  metadata.subject = "video.mp4/plain/1-3";
  metadata.file_md5 = "file";
  metadata.block_md5 = "block";
  metadata.file_size = 100;
  metadata.block_size = 10;
  metadata.create_time = "2026-04-01T00:00:00Z";
  metadata.owner = "tester";
  metadata.local_path = "/video.mp4";
  metadata.mail_folder = "Archive";
  metadata.block_seq = 1;
  metadata.block_count = 3;

  const auto restored = mailfs::core::model::MailBlockMetadata::from_json_text(metadata.to_json_text());
  EXPECT_EQ(restored.subject, metadata.subject);
  EXPECT_EQ(restored.local_path, metadata.local_path);
  EXPECT_EQ(restored.block_seq, 1);
  EXPECT_EQ(restored.block_count, 3);
}

TEST(MailBlockMetadataTest, ParsesSubjectStructure) {
  const auto info = mailfs::core::model::MailBlockMetadata::parse_subject("secret/encrypted/2-5");
  EXPECT_EQ(info.file_name, "secret");
  EXPECT_TRUE(info.encrypted);
  EXPECT_EQ(info.block_seq, 2);
  EXPECT_EQ(info.block_count, 5);
}

TEST(MailBlockMetadataTest, ParsesLegacyKeyValueMetadata) {
  const auto mailbox = std::string(
      "\xE5\x85\xB6\xE4\xBB\x96\xE6\x96\x87\xE4\xBB\xB6\xE5\xA4\xB9\x2F\xE4\xB8\xAA\xE4\xBA\xBA\x70\x63");
  const std::string text =
      "subject:188--login-page.mp4/plain/1-7\r\n"
      "filemd5:ea02326676b2d134a462e957fb614a97\r\n"
      "blockmd5:b60a15a5c3b813483eb6254bbc8791a4\r\n"
      "filesize:212241312\r\n"
      "blocksize:33554432\r\n"
      "createtime:2026-03-08T23:55:47+08:00\r\n"
      "owner:sunshine\r\n"
      "localpath:G:/BaiduNetdiskDownload/demo.mp4\r\n"
      "mailfolder:" + mailbox + "\r\n";

  const auto metadata = mailfs::core::model::MailBlockMetadata::from_legacy_text(text);
  EXPECT_EQ(metadata.subject, "188--login-page.mp4/plain/1-7");
  EXPECT_EQ(metadata.local_path, "G:/BaiduNetdiskDownload/demo.mp4");
  EXPECT_EQ(metadata.mail_folder, mailbox);
  EXPECT_EQ(metadata.file_size, 212241312u);
  EXPECT_EQ(metadata.block_size, 33554432u);
  EXPECT_EQ(metadata.block_seq, 1);
  EXPECT_EQ(metadata.block_count, 7);
  EXPECT_FALSE(metadata.encrypted);
}
