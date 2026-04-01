#include "mailfs/infra/net/secure_socket.hpp"

#include <fstream>
#include <cstring>
#include <stdexcept>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

namespace mailfs::infra::net {

namespace {

std::string mbedtls_error_text(int code) {
  char buffer[256];
  mbedtls_strerror(code, buffer, sizeof(buffer));
  return buffer;
}

void throw_mbedtls_error(const std::string& prefix, int code) {
  throw std::runtime_error(prefix + ": " + mbedtls_error_text(code));
}

}  // namespace

SocketPlatform::SocketPlatform() = default;
SocketPlatform::~SocketPlatform() = default;

struct SecureSocket::Impl {
  mbedtls_net_context net{};
  mbedtls_ssl_context ssl{};
  mbedtls_ssl_config ssl_config{};
  mbedtls_x509_crt ca_chain{};
  mbedtls_entropy_context entropy{};
  mbedtls_ctr_drbg_context ctr_drbg{};

  Impl() {
    mbedtls_net_init(&net);
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&ssl_config);
    mbedtls_x509_crt_init(&ca_chain);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);
  }

  ~Impl() {
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&ssl_config);
    mbedtls_x509_crt_free(&ca_chain);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    mbedtls_net_free(&net);
  }
};

SecureSocket::SecureSocket() : impl_(std::make_unique<Impl>()) {}

SecureSocket::~SecureSocket() {
  close();
}

void SecureSocket::connect(const std::string& host,
                           std::uint16_t port,
                           bool allow_insecure_tls,
                           const std::filesystem::path& ca_cert_file) {
  close();
  impl_ = std::make_unique<Impl>();

  const char* personalization = "mailfs_tls";
  int rc = mbedtls_ctr_drbg_seed(&impl_->ctr_drbg,
                                 mbedtls_entropy_func,
                                 &impl_->entropy,
                                 reinterpret_cast<const unsigned char*>(personalization),
                                 std::strlen(personalization));
  if (rc != 0) {
    throw_mbedtls_error("mbedtls_ctr_drbg_seed failed", rc);
  }

  rc = mbedtls_ssl_config_defaults(
      &impl_->ssl_config,
      MBEDTLS_SSL_IS_CLIENT,
      MBEDTLS_SSL_TRANSPORT_STREAM,
      MBEDTLS_SSL_PRESET_DEFAULT);
  if (rc != 0) {
    throw_mbedtls_error("mbedtls_ssl_config_defaults failed", rc);
  }

  if (!allow_insecure_tls) {
    if (ca_cert_file.empty()) {
      throw std::runtime_error("TLS verification requires ca_cert_file to be set or allow_insecure_tls=true");
    }

    std::ifstream input(ca_cert_file, std::ios::binary);
    if (!input) {
      throw std::runtime_error("failed to open CA certificate file: " + ca_cert_file.u8string());
    }

    std::vector<unsigned char> cert_bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    cert_bytes.push_back('\0');
    rc = mbedtls_x509_crt_parse(&impl_->ca_chain, cert_bytes.data(), cert_bytes.size());
    if (rc < 0) {
      throw_mbedtls_error("mbedtls_x509_crt_parse failed", rc);
    }

    mbedtls_ssl_conf_ca_chain(&impl_->ssl_config, &impl_->ca_chain, nullptr);
  }

  mbedtls_ssl_conf_authmode(&impl_->ssl_config, allow_insecure_tls ? MBEDTLS_SSL_VERIFY_NONE : MBEDTLS_SSL_VERIFY_REQUIRED);
  mbedtls_ssl_conf_rng(&impl_->ssl_config, mbedtls_ctr_drbg_random, &impl_->ctr_drbg);

  const auto port_text = std::to_string(port);
  rc = mbedtls_net_connect(&impl_->net, host.c_str(), port_text.c_str(), MBEDTLS_NET_PROTO_TCP);
  if (rc != 0) {
    throw_mbedtls_error("mbedtls_net_connect failed", rc);
  }

  rc = mbedtls_ssl_setup(&impl_->ssl, &impl_->ssl_config);
  if (rc != 0) {
    throw_mbedtls_error("mbedtls_ssl_setup failed", rc);
  }

  rc = mbedtls_ssl_set_hostname(&impl_->ssl, host.c_str());
  if (rc != 0) {
    throw_mbedtls_error("mbedtls_ssl_set_hostname failed", rc);
  }

  mbedtls_ssl_set_bio(&impl_->ssl, &impl_->net, mbedtls_net_send, mbedtls_net_recv, nullptr);

  while ((rc = mbedtls_ssl_handshake(&impl_->ssl)) != 0) {
    if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
      throw_mbedtls_error("mbedtls_ssl_handshake failed", rc);
    }
  }

  if (!allow_insecure_tls) {
    const auto verify_flags = mbedtls_ssl_get_verify_result(&impl_->ssl);
    if (verify_flags != 0) {
      throw std::runtime_error("TLS certificate verification failed");
    }
  }
}

void SecureSocket::close() noexcept {
  if (impl_) {
    mbedtls_ssl_close_notify(&impl_->ssl);
  }
  impl_.reset();
  read_buffer_.clear();
}

bool SecureSocket::is_open() const noexcept {
  return impl_ != nullptr;
}

void SecureSocket::send_all(const std::string& text) {
  send_all(std::vector<std::uint8_t>(text.begin(), text.end()));
}

void SecureSocket::send_all(const std::vector<std::uint8_t>& bytes) {
  if (!impl_) {
    throw std::runtime_error("socket is not connected");
  }

  std::size_t sent = 0;
  while (sent < bytes.size()) {
    const auto rc = mbedtls_ssl_write(&impl_->ssl,
                                      bytes.data() + sent,
                                      bytes.size() - sent);
    if (rc > 0) {
      sent += static_cast<std::size_t>(rc);
      continue;
    }
    if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
      throw_mbedtls_error("mbedtls_ssl_write failed", rc);
    }
  }
}

std::string SecureSocket::read_more() {
  if (!impl_) {
    throw std::runtime_error("socket is not connected");
  }

  std::string chunk(4096, '\0');
  while (true) {
    const auto rc = mbedtls_ssl_read(&impl_->ssl,
                                     reinterpret_cast<unsigned char*>(chunk.data()),
                                     chunk.size());
    if (rc > 0) {
      chunk.resize(static_cast<std::size_t>(rc));
      return chunk;
    }
    if (rc == 0) {
      throw std::runtime_error("remote side closed the TLS connection");
    }
    if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
      throw_mbedtls_error("mbedtls_ssl_read failed", rc);
    }
  }
}

std::string SecureSocket::read_line() {
  while (true) {
    const auto pos = read_buffer_.find('\n');
    if (pos != std::string::npos) {
      auto line = read_buffer_.substr(0, pos + 1);
      read_buffer_.erase(0, pos + 1);
      if (!line.empty() && line.back() == '\n') {
        line.pop_back();
      }
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      return line;
    }
    read_buffer_ += read_more();
  }
}

std::string SecureSocket::read_exact(std::size_t bytes) {
  while (read_buffer_.size() < bytes) {
    read_buffer_ += read_more();
  }

  auto value = read_buffer_.substr(0, bytes);
  read_buffer_.erase(0, bytes);
  return value;
}

}  // namespace mailfs::infra::net
