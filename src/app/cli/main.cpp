#include <exception>
#include <iostream>
#include <string>
#include <vector>

#include "mailfs/application/mailfs_service.hpp"
#include "mailfs/infra/config/json_config_loader.hpp"
#include "mailfs/infra/imap/imap_client.hpp"
#include "mailfs/infra/storage/sqlite_cache_repository.hpp"

namespace {

void print_usage() {
  std::cout
      << "Usage:\n"
      << "  mailfs_cli [--config path] list-mailboxes\n"
      << "  mailfs_cli [--config path] cache-mailbox <mailbox>\n"
      << "  mailfs_cli [--config path] list-cache <mailbox>\n"
      << "  mailfs_cli [--config path] upload <mailbox> <local-file> <remote-path>\n"
      << "  mailfs_cli [--config path] download <mailbox> <remote-path> <output-file>\n";
}

}  // namespace

int main(int argc, char** argv) {
  try {
    std::vector<std::string> args(argv + 1, argv + argc);
    std::string config_path = "config/mailfs.json";

    if (args.size() >= 2 && args[0] == "--config") {
      config_path = args[1];
      args.erase(args.begin(), args.begin() + 2);
    }

    if (args.empty()) {
      print_usage();
      return 1;
    }

    auto config = mailfs::infra::config::JsonConfigLoader::load(config_path);
    mailfs::infra::storage::SQLiteCacheRepository repository(config.database_path);
    mailfs::infra::imap::ImapClient client;
    mailfs::application::MailfsService service(config, client, repository);

    const auto& command = args[0];
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
      service.upload_file(args[1], args[2], args[3]);
      std::cout << "upload complete\n";
      return 0;
    }

    if (command == "download") {
      if (args.size() != 4) {
        print_usage();
        return 1;
      }
      service.download_file(args[1], args[2], args[3]);
      std::cout << "download complete\n";
      return 0;
    }

    print_usage();
    return 1;
  } catch (const std::exception& ex) {
    std::cerr << "error: " << ex.what() << '\n';
    return 2;
  }
}
