#include <string>

#include <gtest/gtest.h>

#include "mailfs/infra/platform/utf8.hpp"

TEST(Utf8Test, NarrowArgvPassesThroughUtf8Bytes) {
  char arg0[] = "mailfs_cli";
  char arg1[] = "--config";
  char arg2[] = u8"\u914d\u7f6e/mailfs.json";
  char* argv[] = {arg0, arg1, arg2};

  const auto args = mailfs::infra::platform::argv_to_utf8(3, argv);
  ASSERT_EQ(args.size(), 3u);
  EXPECT_EQ(args[2], u8"\u914d\u7f6e/mailfs.json");
}

#ifdef _WIN32
TEST(Utf8Test, WideArgvConvertsToUtf8) {
  wchar_t arg0[] = L"mailfs_cli";
  wchar_t arg1[] = L"download";
  wchar_t arg2[] = L"\u6536\u4ef6\u7bb1";
  wchar_t* argv[] = {arg0, arg1, arg2};

  const auto args = mailfs::infra::platform::argv_to_utf8(3, argv);
  ASSERT_EQ(args.size(), 3u);
  EXPECT_EQ(args[2], u8"\u6536\u4ef6\u7bb1");
}
#endif
