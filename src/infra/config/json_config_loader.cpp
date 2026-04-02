#include "mailfs/infra/config/json_config_loader.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace mailfs::infra::config {

namespace {

std::string lower_copy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

const nlohmann::json* find_object(const nlohmann::json& root, std::string_view key) {
  const auto it = root.find(std::string(key));
  if (it == root.end() || !it->is_object()) {
    return nullptr;
  }
  return &(*it);
}

template <typename T>
void load_value(const nlohmann::json& root,
                std::string_view section,
                std::string_view key,
                std::string_view legacy_key,
                T& target) {
  if (const auto* object = find_object(root, section)) {
    const auto it = object->find(std::string(key));
    if (it != object->end()) {
      target = it->get<T>();
      return;
    }
  }

  if (!legacy_key.empty()) {
    const auto it = root.find(std::string(legacy_key));
    if (it != root.end()) {
      target = it->get<T>();
    }
  }
}

}  // namespace

core::model::AppConfig JsonConfigLoader::load(const std::filesystem::path& path) {
  core::model::AppConfig config;

  if (!std::filesystem::exists(path)) {
    return config;
  }

  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("failed to open config file: " + path.u8string());
  }

  nlohmann::json root;
  input >> root;

  std::string imap_server;
  load_value(root, "imap", "server", "imap_server", imap_server);
  if (!imap_server.empty()) {
    const auto endpoint = imap_server;
    const auto colon = endpoint.find(':');
    if (colon == std::string::npos) {
      config.imap_host = endpoint;
    } else {
      config.imap_host = endpoint.substr(0, colon);
      config.imap_port = static_cast<std::uint16_t>(std::stoi(endpoint.substr(colon + 1)));
    }
  }

  load_value(root, "imap", "credential_file", "credential_file", config.credential_file);
  load_value(root, "imap", "ca_cert_file", "ca_cert_file", config.ca_cert_file);
  load_value(root, "imap", "allow_insecure_tls", "allow_insecure_tls", config.allow_insecure_tls);

  load_value(root, "logging", "level", "log_level", config.log_level);
  load_value(root, "logging", "file", "log_file", config.log_file);
  load_value(root, "logging", "to_stderr", "log_to_stderr", config.log_to_stderr);
  load_value(root, "logging", "max_file_size", "log_max_file_size", config.log_max_file_size);
  load_value(root, "logging", "max_files", "log_max_files", config.log_max_files);

  load_value(root, "identity", "email_name", "email_name", config.email_name);
  load_value(root, "identity", "owner_name", "owner_name", config.owner_name);

  load_value(root, "mailbox", "default", "default_mailbox", config.default_mailbox);
  load_value(root, "storage", "download_dir", "download_dir", config.download_dir);
  load_value(root, "storage", "database_path", "database_path", config.database_path);

  load_value(root, "http", "listen_addr", "http_listen_addr", config.http_listen_addr);
  load_value(root, "http", "copy_addr", "http_copy_addr", config.http_copy_addr);

  load_value(root, "cache", "default_block_size", "default_block_size", config.default_block_size);
  load_value(root, "cache", "fetch_batch_size", "cache_fetch_batch_size", config.cache_fetch_batch_size);

  if (const auto* cache = find_object(root, "cache")) {
    if (const auto it = cache->find("block_sizes"); it != cache->end()) {
      for (auto item = it->begin(); item != it->end(); ++item) {
        config.block_sizes.emplace(lower_copy(item.key()), item.value().get<std::size_t>());
      }
    }
  } else if (const auto it = root.find("block_sizes"); it != root.end()) {
    for (auto item = it->begin(); item != it->end(); ++item) {
      config.block_sizes.emplace(lower_copy(item.key()), item.value().get<std::size_t>());
    }
  }

  if (const auto* mailbox = find_object(root, "mailbox")) {
    if (const auto it = mailbox->find("allowed_folders"); it != mailbox->end()) {
      config.allowed_folders = it->get<std::vector<std::string>>();
    }
  } else if (const auto it = root.find("allowed_folders"); it != root.end()) {
    config.allowed_folders = it->get<std::vector<std::string>>();
  }

  if (const auto* upload = find_object(root, "upload")) {
    if (const auto it = upload->find("ignore_extensions"); it != upload->end()) {
      config.ignore_extensions = it->get<std::vector<std::string>>();
    }
  } else if (const auto it = root.find("ignore_extensions"); it != root.end()) {
    config.ignore_extensions = it->get<std::vector<std::string>>();
  }

  for (auto& ext : config.ignore_extensions) {
    ext = lower_copy(ext);
  }

  if (config.log_max_file_size == 0) {
    config.log_max_file_size = core::model::AppConfig{}.log_max_file_size;
  }
  if (config.log_max_files < 0) {
    config.log_max_files = 0;
  }
  if (config.cache_fetch_batch_size == 0) {
    config.cache_fetch_batch_size = 1;
  }
  if (config.default_block_size == 0) {
    config.default_block_size = core::model::AppConfig{}.default_block_size;
  }

  return config;
}

}  // namespace mailfs::infra::config
