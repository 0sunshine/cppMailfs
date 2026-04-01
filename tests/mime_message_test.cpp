#include <gtest/gtest.h>

#include "mailfs/core/mime/mime_message.hpp"

TEST(MimeMessageTest, MultipartRoundTripPreservesBodies) {
  mailfs::core::mime::MimeMessage message;
  message.headers["Subject"] = "demo";

  mailfs::core::mime::MimePart json_part;
  json_part.headers["Content-Type"] = "application/json";
  json_part.headers["Content-Transfer-Encoding"] = "base64";
  json_part.body = {'{', '}', '\n'};

  mailfs::core::mime::MimePart file_part;
  file_part.headers["Content-Type"] = "application/octet-stream";
  file_part.headers["Content-Disposition"] = "attachment; filename=\"block.bin\"";
  file_part.headers["Content-Transfer-Encoding"] = "base64";
  file_part.body = {0x00, 0x01, 0x02, 0xff};

  message.parts.push_back(json_part);
  message.parts.push_back(file_part);

  const auto raw = message.render_multipart_mixed("unit-boundary");
  const auto parsed = mailfs::core::mime::MimeMessage::parse(raw);

  ASSERT_EQ(parsed.parts.size(), 2u);
  EXPECT_EQ(parsed.parts[0].body, json_part.body);
  EXPECT_EQ(parsed.parts[1].body, file_part.body);
}
