#pragma once

namespace mailfs::infra::net {

class SocketPlatform {
 public:
  SocketPlatform();
  ~SocketPlatform();

  SocketPlatform(const SocketPlatform&) = delete;
  SocketPlatform& operator=(const SocketPlatform&) = delete;
};

}  // namespace mailfs::infra::net
