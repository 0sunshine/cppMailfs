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
      << "  mailfs_cli [--config path] upload <mailbox> <local-file>\n"
      << "  mailfs_cli [--config path] download <mailbox> <local-path>\n";
}

void print_count_progress(const std::string& label, std::size_t done, std::size_t total) {
  std::cerr << '\r' << '[' << label << "] " << done << '/' << total;
  if (total == 0 || done >= total) {
    std::cerr << '\n';
  }
}

void print_block_progress(const std::string& label,
                          std::int64_t current_block,
                          std::int64_t total_blocks,
                          const std::string& file_name) {
  std::cerr << '\r' << '[' << label << "] " << file_name << ' ' << current_block << '/' << total_blocks;
  if (total_blocks == 0 || current_block >= total_blocks) {
    std::cerr << '\n';
  }
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
      const auto count = service.cache_mailbox(args[1], [](std::size_t done, std::size_t total) {
        print_count_progress("cache-mailbox", done, total);
      });
      std::cout << "cached messages: " << count << '\n';
      return 0;
    }

    if (command == "list-cache") {
      if (args.size() != 2) {
        print_usage();
        return 1;
      }
      const auto files = service.list_cached_files(args[1], [](std::size_t done,
                                                               std::size_t total,
                                                               const std::string& local_path) {
        std::cerr << '\r' << "[list-cache] " << done << '/' << total << ' ' << local_path;
        if (total == 0 || done >= total) {
          std::cerr << '\n';
        }
      });
      for (const auto& file : files) {
        std::cout << file.local_path << " blocks=" << file.block_count << " size=" << file.file_size << '\n';
      }
      return 0;
    }

    if (command == "upload") {
      if (args.size() != 3) {
        print_usage();
        return 1;
      }
      service.upload_file(args[1], std::filesystem::u8path(args[2]), [](std::int64_t current_block,
                                                                        std::int64_t total_blocks,
                                                                        const std::string& file_name) {
        print_block_progress("upload", current_block, total_blocks, file_name);
      });
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
      if (args.size() != 3) {
        print_usage();
        return 1;
      }
      const auto output_path = service.download_file(args[1], args[2], [](std::int64_t current_block,
                                                                          std::int64_t total_blocks,
                                                                          const std::string& file_name) {
        print_block_progress("download", current_block, total_blocks, file_name);
      });
      std::cout << "download complete: " << output_path.u8string() << '\n';
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
