#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "mailfs/application/mailfs_service.hpp"
#include "mailfs/infra/config/json_config_loader.hpp"
#include "mailfs/infra/imap/imap_client.hpp"
#include "mailfs/infra/logging/logger.hpp"
#include "mailfs/infra/platform/utf8.hpp"
#include "mailfs/infra/storage/sqlite_cache_repository.hpp"

namespace {

void print_usage() {
  std::cout
      << "Usage:\n"
      << "  mailfs_cli [--config path] list-mailboxes\n"
      << "  mailfs_cli [--config path] cache-mailbox <mailbox>\n"
      << "  mailfs_cli [--config path] list-cache <mailbox>\n"
      << "  mailfs_cli [--config path] delete-uid <mailbox> <uid>\n"
      << "  mailfs_cli [--config path] upload <mailbox> <local-file> <remote-path>\n"
      << "  mailfs_cli [--config path] download <mailbox> <remote-path> <output-file>\n";
}

int run_cli(std::vector<std::string> args) {
  try {
    mailfs::infra::logging::Logger::instance().configure({});
    std::string config_path = "config/mailfs.json";

    if (args.size() >= 2 && args[0] == "--config") {
      config_path = args[1];
      args.erase(args.begin(), args.begin() + 2);
    }

    if (args.empty()) {
      print_usage();
      return 1;
    }

    auto config = mailfs::infra::config::JsonConfigLoader::load(std::filesystem::u8path(config_path));
    mailfs::infra::logging::Logger::instance().configure({
        mailfs::infra::logging::parse_log_level(config.log_level),
        std::filesystem::u8path(config.log_file),
        config.log_to_stderr,
    });
    mailfs::infra::logging::log_info("cli", "loaded config from " + config_path);
    mailfs::infra::storage::SQLiteCacheRepository repository(std::filesystem::u8path(config.database_path));
    mailfs::infra::imap::ImapClient client;
    mailfs::application::MailfsService service(config, client, repository);

    const auto& command = args[0];
    mailfs::infra::logging::log_info("cli", "executing command " + command);
    if (command == "list-mailboxes") {
      for (const auto& mailbox : service.list_mailboxes()) {
        std::cout << mailbox << '\n';
      }
      return 0;
    }

    if (command == "cache-mailbox") {
      if (args.size() != 2) {
        print_usage();
        return 1;
      }
      const auto count = service.cache_mailbox(args[1]);
      std::cout << "cached messages: " << count << '\n';
      return 0;
    }

    if (command == "list-cache") {
      if (args.size() != 2) {
        print_usage();
        return 1;
      }
      const auto files = service.list_cached_files(args[1]);
      for (const auto& file : files) {
        std::cout << file.local_path << " blocks=" << file.block_count << " size=" << file.file_size << '\n';
      }
      return 0;
    }

    if (command == "upload") {
      if (args.size() != 4) {
        print_usage();
        return 1;
      }
      service.upload_file(args[1], std::filesystem::u8path(args[2]), args[3]);
      std::cout << "upload complete\n";
      return 0;
    }

    if (command == "delete-uid") {
      if (args.size() != 3) {
        print_usage();
        return 1;
      }
      service.delete_message_uid(args[1], std::stoull(args[2]));
      std::cout << "delete complete\n";
      return 0;
    }

    if (command == "download") {
      if (args.size() != 4) {
        print_usage();
        return 1;
      }
      service.download_file(args[1], args[2], std::filesystem::u8path(args[3]));
      std::cout << "download complete\n";
      return 0;
    }

    print_usage();
    return 1;
  } catch (const std::exception& ex) {
    mailfs::infra::logging::log_error("cli", ex.what());
    std::cerr << "error: " << ex.what() << '\n';
    return 2;
  }
}

}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
  mailfs::infra::platform::prepare_console_utf8();
  auto args = mailfs::infra::platform::argv_to_utf8(argc, argv);
  if (!args.empty()) {
    args.erase(args.begin());
  }
  return run_cli(std::move(args));
}
#else
int main(int argc, char** argv) {
  mailfs::infra::platform::prepare_console_utf8();
  auto args = mailfs::infra::platform::argv_to_utf8(argc, argv);
  if (!args.empty()) {
    args.erase(args.begin());
  }
  return run_cli(std::move(args));
}
#endif
