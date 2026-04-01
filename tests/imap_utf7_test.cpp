#include <gtest/gtest.h>

#include "mailfs/infra/imap/imap_utf7.hpp"

TEST(ImapUtf7Test, DecodesModifiedUtf7MailboxNames) {
  EXPECT_EQ(mailfs::infra::imap::decode_imap_utf7("&UXZO1mWHTvZZOQ-"), u8"\u5176\u4ed6\u6587\u4ef6\u5939");
  EXPECT_EQ(mailfs::infra::imap::decode_imap_utf7("&UXZO1mWHTvZZOQ-/&bUuL1Q-"),
            u8"\u5176\u4ed6\u6587\u4ef6\u5939/\u6d4b\u8bd5");
  EXPECT_EQ(mailfs::infra::imap::decode_imap_utf7("Sent Messages"), "Sent Messages");
  EXPECT_EQ(mailfs::infra::imap::decode_imap_utf7("&-test"), "&test");
}

TEST(ImapUtf7Test, EncodesMailboxNamesToModifiedUtf7) {
  EXPECT_EQ(mailfs::infra::imap::encode_imap_utf7(u8"\u5176\u4ed6\u6587\u4ef6\u5939"), "&UXZO1mWHTvZZOQ-");
  EXPECT_EQ(mailfs::infra::imap::encode_imap_utf7(u8"\u5176\u4ed6\u6587\u4ef6\u5939/\u6d4b\u8bd5"),
            "&UXZO1mWHTvZZOQ-/&bUuL1Q-");
  EXPECT_EQ(mailfs::infra::imap::encode_imap_utf7("Sent Messages"), "Sent Messages");
  EXPECT_EQ(mailfs::infra::imap::encode_imap_utf7("&test"), "&-test");
}
