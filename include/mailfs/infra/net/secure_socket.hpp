#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "mailfs/infra/net/platform.hpp"

namespace mailfs::infra::net {

class SecureSocket {
 public:
  SecureSocket();
  ~SecureSocket();

  SecureSocket(const SecureSocket&) = delete;
  SecureSocket& operator=(const SecureSocket&) = delete;

  void connect(const std::string& host,
               std::uint16_t port,
               bool allow_insecure_tls,
               const std::filesystem::path& ca_cert_file = {});
  void close() noexcept;
  [[nodiscard]] bool is_open() const noexcept;
  void send_all(const std::string& text);
  void send_all(const std::vector<std::uint8_t>& bytes);
  [[nodiscard]] std::string read_line();
 [[nodiscard]] std::string read_exact(std::size_t bytes);

 private:
  struct Impl;

  SocketPlatform platform_;
  std::unique_ptr<Impl> impl_;
  std::string read_buffer_;

  [[nodiscard]] std::string read_more();
};

}  // namespace mailfs::infra::net
