#include <cstdint>
#include <map>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "mailfs/application/http_imap_download_server.hpp"

namespace {

std::string base64_url_encode(std::string_view input) {
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

  std::string output;
  output.reserve(((input.size() + 2) / 3) * 4);
  std::size_t index = 0;
  while (index + 3 <= input.size()) {
    const auto chunk = (static_cast<std::uint32_t>(static_cast<unsigned char>(input[index])) << 16) |
                       (static_cast<std::uint32_t>(static_cast<unsigned char>(input[index + 1])) << 8) |
                       static_cast<std::uint32_t>(static_cast<unsigned char>(input[index + 2]));
    output.push_back(kAlphabet[(chunk >> 18) & 0x3F]);
    output.push_back(kAlphabet[(chunk >> 12) & 0x3F]);
    output.push_back(kAlphabet[(chunk >> 6) & 0x3F]);
    output.push_back(kAlphabet[chunk & 0x3F]);
    index += 3;
  }

  const auto remaining = input.size() - index;
  if (remaining == 1) {
    const auto chunk = static_cast<std::uint32_t>(static_cast<unsigned char>(input[index])) << 16;
    output.push_back(kAlphabet[(chunk >> 18) & 0x3F]);
    output.push_back(kAlphabet[(chunk >> 12) & 0x3F]);
  } else if (remaining == 2) {
    const auto chunk = (static_cast<std::uint32_t>(static_cast<unsigned char>(input[index])) << 16) |
                       (static_cast<std::uint32_t>(static_cast<unsigned char>(input[index + 1])) << 8);
    output.push_back(kAlphabet[(chunk >> 18) & 0x3F]);
    output.push_back(kAlphabet[(chunk >> 12) & 0x3F]);
    output.push_back(kAlphabet[(chunk >> 6) & 0x3F]);
  }

  return output;
}

std::string make_target(const std::string& mailbox, const std::string& local_path) {
  return "/httptoimap?imapdir=" + base64_url_encode(mailbox) + "&localpath=" + base64_url_encode(local_path);
}

std::string collect_stream_body(const mailfs::application::HttpResponse& response) {
  std::string body;
  if (response.stream_body) {
    const auto ok = response.stream_body([&](std::string_view chunk) {
      body.append(chunk.data(), chunk.size());
      return true;
    });
    EXPECT_TRUE(ok);
  } else {
    body = response.body;
  }
  return body;
}

std::string header_value(const mailfs::application::HttpResponse& response, const std::string& name) {
  for (const auto& [key, value] : response.headers) {
    if (key == name) {
      return value;
    }
  }
  return {};
}

class FakeDownloadProvider final : public mailfs::application::IHttpImapDownloadProvider {
 public:
  mailfs::core::model::CachedFileRecord resolve_file(const std::string&, const std::string&) override {
    return file;
  }

  mailfs::application::DownloadedBlockData fetch_block(
      const std::string&,
      const mailfs::core::model::CachedFileRecord&,
      const mailfs::core::model::CachedBlockRecord& block) override {
    if (fetch_delay_ms > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(fetch_delay_ms));
    }
    std::lock_guard<std::mutex> lock(fetch_count_mutex);
    ++fetch_count[block.uid];
    return blocks.at(block.uid);
  }

  int fetch_count_for(std::uint64_t uid) {
    std::lock_guard<std::mutex> lock(fetch_count_mutex);
    const auto it = fetch_count.find(uid);
    return it == fetch_count.end() ? 0 : it->second;
  }

  mailfs::core::model::CachedFileRecord file;
  std::map<std::uint64_t, mailfs::application::DownloadedBlockData> blocks;
  std::map<std::uint64_t, int> fetch_count;
  int fetch_delay_ms = 0;
  std::mutex fetch_count_mutex;
};

}  // namespace

TEST(HttpImapDownloadServerTest, StreamsWholeFileAndCachesFirstBlockForSizeProbe) {
  auto provider = std::make_shared<FakeDownloadProvider>();
  provider->file.mail_folder = "Archive";
  provider->file.local_path = "/demo/video.bin";
  provider->file.block_count = 2;
  provider->file.file_size = 0;
  provider->file.blocks = {
      {1, 11, "md5-1", 3},
      {2, 12, "md5-2", 3},
  };

  mailfs::core::model::MailBlockMetadata meta1;
  meta1.file_size = 6;
  provider->blocks.emplace(11, mailfs::application::DownloadedBlockData{meta1, {'a', 'b', 'c'}});
  provider->blocks.emplace(12, mailfs::application::DownloadedBlockData{meta1, {'d', 'e', 'f'}});

  mailfs::core::model::AppConfig config;
  mailfs::application::HttpImapDownloadServer server(config, [provider]() { return provider; });

  mailfs::application::HttpRequest request;
  request.method = "GET";
  request.target = make_target("Archive", "/demo/video.bin");

  const auto response = server.handle_request(request);

  EXPECT_EQ(response.status_code, 200);
  EXPECT_EQ(header_value(response, "Content-Length"), "6");
  EXPECT_EQ(collect_stream_body(response), "abcdef");
  EXPECT_EQ(provider->fetch_count_for(11), 1);
  EXPECT_EQ(provider->fetch_count_for(12), 1);
}

TEST(HttpImapDownloadServerTest, SupportsSingleRangeRequest) {
  auto provider = std::make_shared<FakeDownloadProvider>();
  provider->file.mail_folder = "Archive";
  provider->file.local_path = "/demo/video.bin";
  provider->file.block_count = 2;
  provider->file.file_size = 6;
  provider->file.blocks = {
      {1, 11, "md5-1", 3},
      {2, 12, "md5-2", 3},
  };

  mailfs::core::model::MailBlockMetadata meta;
  meta.file_size = 6;
  provider->blocks.emplace(11, mailfs::application::DownloadedBlockData{meta, {'a', 'b', 'c'}});
  provider->blocks.emplace(12, mailfs::application::DownloadedBlockData{meta, {'d', 'e', 'f'}});

  mailfs::core::model::AppConfig config;
  mailfs::application::HttpImapDownloadServer server(config, [provider]() { return provider; });

  mailfs::application::HttpRequest request;
  request.method = "GET";
  request.target = make_target("Archive", "/demo/video.bin");
  request.headers.emplace("Range", "bytes=2-4");

  const auto response = server.handle_request(request);

  EXPECT_EQ(response.status_code, 206);
  EXPECT_EQ(header_value(response, "Content-Range"), "bytes 2-4/6");
  EXPECT_EQ(header_value(response, "Content-Length"), "3");
  EXPECT_EQ(collect_stream_body(response), "cde");
}

TEST(HttpImapDownloadServerTest, RejectsMissingQueryParameters) {
  auto provider = std::make_shared<FakeDownloadProvider>();
  mailfs::core::model::AppConfig config;
  mailfs::application::HttpImapDownloadServer server(config, [provider]() { return provider; });

  mailfs::application::HttpRequest request;
  request.method = "GET";
  request.target = "/httptoimap";

  const auto response = server.handle_request(request);

  EXPECT_EQ(response.status_code, 400);
  EXPECT_EQ(response.body, "missing imapdir or localpath parameter\n");
}

TEST(HttpImapDownloadServerTest, StopsSequentialPrefetchAfterClientDisconnect) {
  auto provider = std::make_shared<FakeDownloadProvider>();
  provider->fetch_delay_ms = 20;
  provider->file.mail_folder = "Archive";
  provider->file.local_path = "/demo/video.mp4";
  provider->file.block_count = 3;
  provider->file.file_size = 9;
  provider->file.blocks = {
      {1, 11, "md5-1", 3},
      {2, 12, "md5-2", 3},
      {3, 13, "md5-3", 3},
  };

  mailfs::core::model::MailBlockMetadata meta;
  meta.file_size = 9;
  provider->blocks.emplace(11, mailfs::application::DownloadedBlockData{meta, {'a', 'b', 'c'}});
  provider->blocks.emplace(12, mailfs::application::DownloadedBlockData{meta, {'d', 'e', 'f'}});
  provider->blocks.emplace(13, mailfs::application::DownloadedBlockData{meta, {'g', 'h', 'i'}});

  mailfs::core::model::AppConfig config;
  mailfs::application::HttpImapDownloadServer server(config, [provider]() { return provider; });

  mailfs::application::HttpRequest request;
  request.method = "GET";
  request.target = make_target("Archive", "/demo/video.mp4");

  const auto response = server.handle_request(request);

  std::string body;
  ASSERT_TRUE(response.stream_body);
  const auto stream_ok = response.stream_body([&](std::string_view chunk) {
    body.append(chunk.data(), chunk.size());
    return false;
  });
  EXPECT_FALSE(stream_ok);
  EXPECT_EQ(body, "abc");

  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  EXPECT_EQ(provider->fetch_count_for(11), 1);
  EXPECT_EQ(provider->fetch_count_for(12), 1);
  EXPECT_EQ(provider->fetch_count_for(13), 0);
}
