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

  if (const auto server_it = root.find("imap_server"); server_it != root.end()) {
    const auto endpoint = server_it->get<std::string>();
    const auto colon = endpoint.find(':');
    if (colon == std::string::npos) {
      config.imap_host = endpoint;
    } else {
      config.imap_host = endpoint.substr(0, colon);
      config.imap_port = static_cast<std::uint16_t>(std::stoi(endpoint.substr(colon + 1)));
    }
  }

  config.credential_file = root.value("credential_file", config.credential_file);
  config.ca_cert_file = root.value("ca_cert_file", config.ca_cert_file);
  config.email_name = root.value("email_name", config.email_name);
  config.mailbox_prefix = root.value("mailbox_prefix", config.mailbox_prefix);
  config.database_path = root.value("database_path", config.database_path);
  config.default_block_size = root.value("default_block_size", config.default_block_size);
  config.allow_insecure_tls = root.value("allow_insecure_tls", config.allow_insecure_tls);

  if (const auto it = root.find("block_sizes"); it != root.end()) {
    for (auto item = it->begin(); item != it->end(); ++item) {
      config.block_sizes.emplace(lower_copy(item.key()), item.value().get<std::size_t>());
    }
  }

  if (const auto it = root.find("allowed_folders"); it != root.end()) {
    config.allowed_folders = it->get<std::vector<std::string>>();
  }

  if (const auto it = root.find("ignore_extensions"); it != root.end()) {
    config.ignore_extensions = it->get<std::vector<std::string>>();
    for (auto& ext : config.ignore_extensions) {
      ext = lower_copy(ext);
    }
  }

  return config;
}

}  // namespace mailfs::infra::config
