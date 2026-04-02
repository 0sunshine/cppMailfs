#include <gtest/gtest.h>

#include "mailfs/infra/imap/imap_response_parser.hpp"

TEST(ImapResponseParserTest, ParsesLiteralAndTaggedStatus) {
  const auto literal = mailfs::infra::imap::ImapResponseParser::literal_size_from_line("* 23 FETCH (UID 9 BODY[] {123}");
  ASSERT_TRUE(literal.has_value());
  EXPECT_EQ(*literal, 123u);

  const auto status =
      mailfs::infra::imap::ImapResponseParser::parse_tagged_status("A0001 OK SELECT completed", "A0001");
  ASSERT_TRUE(status.has_value());
  EXPECT_EQ(status->status, "OK");
  EXPECT_EQ(status->text, "SELECT completed");

  const auto append_uid =
      mailfs::infra::imap::ImapResponseParser::parse_append_uid("[APPENDUID 38505 3955] APPEND completed");
  ASSERT_TRUE(append_uid.has_value());
  EXPECT_EQ(*append_uid, 3955u);
}

TEST(ImapResponseParserTest, ParsesListSearchAndFetchLines) {
  const auto mailbox =
      mailfs::infra::imap::ImapResponseParser::parse_list_mailbox("* LIST (\\HasNoChildren) \"/\" \"Archive\"");
  ASSERT_TRUE(mailbox.has_value());
  EXPECT_EQ(*mailbox, "Archive");

  const auto encoded_mailbox =
      mailfs::infra::imap::ImapResponseParser::parse_list_mailbox("* LIST (\\HasNoChildren) \"/\" \"&UXZO1mWHTvZZOQ-\"");
  ASSERT_TRUE(encoded_mailbox.has_value());
  EXPECT_EQ(*encoded_mailbox, u8"\u5176\u4ed6\u6587\u4ef6\u5939");

  const auto mailbox_name = mailfs::infra::imap::ImapResponseParser::parse_list_mailbox_name(
      "* LIST (\\HasNoChildren) \"/\" \"&UXZO1mWHTvZZOQ-/&TipOug-pc\"");
  ASSERT_TRUE(mailbox_name.has_value());
  EXPECT_EQ(mailbox_name->raw_name, "&UXZO1mWHTvZZOQ-/&TipOug-pc");
  EXPECT_EQ(mailbox_name->decoded_name, u8"\u5176\u4ed6\u6587\u4ef6\u5939/\u4e2a\u4ebapc");

  const auto uids = mailfs::infra::imap::ImapResponseParser::parse_search_uids("* SEARCH 10 11 15");
  ASSERT_EQ(uids.size(), 3u);
  EXPECT_EQ(uids[0], 10u);
  EXPECT_EQ(uids[2], 15u);

  const auto fetch_uid = mailfs::infra::imap::ImapResponseParser::parse_fetch_uid("* 2 FETCH (UID 42 BODY[] {10}");
  ASSERT_TRUE(fetch_uid.has_value());
  EXPECT_EQ(*fetch_uid, 42u);
}
