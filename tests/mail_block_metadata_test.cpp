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
