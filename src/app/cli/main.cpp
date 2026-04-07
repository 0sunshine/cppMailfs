#include <algorithm>
#include <cctype>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "mailfs/application/http_imap_download_server.hpp"
#include "mailfs/application/mailfs_service.hpp"
#include "mailfs/infra/config/json_config_loader.hpp"
#include "mailfs/infra/imap/imap_client.hpp"
#include "mailfs/infra/logging/logger.hpp"
#include "mailfs/infra/platform/utf8.hpp"
#include "mailfs/infra/storage/sqlite_cache_repository.hpp"

namespace {

std::string normalize_slashes(std::string text) {
  std::replace(text.begin(), text.end(), '\\', '/');
  return text;
}

std::string last_segment(std::string text) {
  text = normalize_slashes(std::move(text));
  while (!text.empty() && text.back() == '/') {
    text.pop_back();
  }
  const auto slash = text.find_last_of('/');
  return slash == std::string::npos ? text : text.substr(slash + 1);
}

std::string lower_copy(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return text;
}

bool looks_like_local_path(std::string_view text) {
  if (text.size() >= 2 && std::isalpha(static_cast<unsigned char>(text[0])) != 0 && text[1] == ':') {
    return true;
  }
  return text.rfind("/", 0) == 0 || text.rfind("\\", 0) == 0 || text.rfind("./", 0) == 0 ||
         text.rfind(".\\", 0) == 0 || text.rfind("../", 0) == 0 || text.rfind("..\\", 0) == 0 ||
         text.find('\\') != std::string_view::npos;
}

bool looks_like_json_output(std::string_view text) {
  return lower_copy(std::filesystem::u8path(std::string(text)).extension().u8string()) == ".json";
}

std::string resolve_mailbox(const mailfs::core::model::AppConfig& config,
                            std::optional<std::string> explicit_mailbox,
                            std::string_view command) {
  if (explicit_mailbox.has_value() && !explicit_mailbox->empty()) {
    return *explicit_mailbox;
  }
  if (!config.default_mailbox.empty()) {
    return config.default_mailbox;
  }
  throw std::runtime_error("mailbox is required for " + std::string(command) +
                           " because mailbox.default is not configured");
}

void print_usage() {
  std::cout
      << "Usage:\n"
      << "  mailfs_cli [--config path] list-mailboxes\n"
      << "  mailfs_cli [--config path] cache-mailbox [mailbox]\n"
      << "  mailfs_cli [--config path] list-cache [mailbox]\n"
      << "  mailfs_cli [--config path] check-integrity [mailbox] [local-path-prefix]\n"
      << "  mailfs_cli [--config path] dedup-mailbox [mailbox] [local-path-prefix]\n"
      << "  mailfs_cli [--config path] delete-uid [mailbox] <uid>\n"
      << "  mailfs_cli [--config path] export-playlist [mailbox] [output-json] [local-path-prefix]\n"
      << "  mailfs_cli [--config path] upload [mailbox] <local-file-or-directory>\n"
      << "  mailfs_cli [--config path] download [mailbox] <local-path>\n"
      << "  mailfs_cli [--config path] serve-http [--listen addr]\n"
      << "If [mailbox] is omitted, mailbox.default from the config is used.\n";
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
        config.log_max_file_size,
        config.log_max_files,
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
      if (args.size() > 2) {
        print_usage();
        return 1;
      }
      const auto mailbox =
          resolve_mailbox(config, args.size() == 2 ? std::optional<std::string>(args[1]) : std::nullopt, command);
      const auto count = service.cache_mailbox(mailbox, [](std::size_t done, std::size_t total) {
        print_count_progress("cache-mailbox", done, total);
      });
      std::cout << "cached messages: " << count << '\n';
      return 0;
    }

    if (command == "list-cache") {
      if (args.size() > 2) {
        print_usage();
        return 1;
      }
      const auto mailbox =
          resolve_mailbox(config, args.size() == 2 ? std::optional<std::string>(args[1]) : std::nullopt, command);
      const auto files = service.list_cached_files(mailbox, [](std::size_t done,
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

    if (command == "check-integrity") {
      if (args.size() > 3) {
        print_usage();
        return 1;
      }
      std::optional<std::string> explicit_mailbox;
      std::string prefix;
      if (args.size() == 2) {
        if (!config.default_mailbox.empty() && looks_like_local_path(args[1])) {
          prefix = args[1];
        } else {
          explicit_mailbox = args[1];
        }
      } else if (args.size() == 3) {
        explicit_mailbox = args[1];
        prefix = args[2];
      }
      const auto mailbox = resolve_mailbox(config, explicit_mailbox, command);
      const auto results = service.check_cached_integrity(mailbox, prefix);
      std::size_t ok_count = 0;
      std::size_t broken_count = 0;
      for (const auto& result : results) {
        if (result.ok) {
          ++ok_count;
          continue;
        }
        ++broken_count;
        std::cout << "BROKEN " << result.file.local_path << " cached=" << result.cached_blocks
                  << " expected=" << result.expected_blocks << '\n';
      }
      std::cout << "integrity ok=" << ok_count << " broken=" << broken_count << " total=" << results.size() << '\n';
      return 0;
    }

    if (command == "dedup-mailbox") {
      if (args.size() > 3) {
        print_usage();
        return 1;
      }
      std::optional<std::string> explicit_mailbox;
      std::string prefix;
      if (args.size() == 2) {
        if (!config.default_mailbox.empty() && looks_like_local_path(args[1])) {
          prefix = args[1];
        } else {
          explicit_mailbox = args[1];
        }
      } else if (args.size() == 3) {
        explicit_mailbox = args[1];
        prefix = args[2];
      }
      const auto mailbox = resolve_mailbox(config, explicit_mailbox, command);
      const auto results = service.deduplicate_mailbox(mailbox, prefix, [](std::size_t done, std::size_t total) {
        print_count_progress("dedup-mailbox-scan", done, total);
      });
      std::size_t deleted_uid_count = 0;
      for (const auto& result : results) {
        deleted_uid_count += result.deleted_uids.size();
        std::cout << "DEDUP " << result.local_path << " keep=" << result.kept_uids.size()
                  << " delete=" << result.deleted_uids.size() << '\n';
      }
      std::cout << "dedup complete: files=" << results.size() << " deleted_uids=" << deleted_uid_count << '\n';
      return 0;
    }

    if (command == "upload") {
      if (args.size() != 2 && args.size() != 3) {
        print_usage();
        return 1;
      }
      const auto mailbox =
          resolve_mailbox(config, args.size() == 3 ? std::optional<std::string>(args[1]) : std::nullopt, command);
      const auto local_path = std::filesystem::u8path(args.size() == 3 ? args[2] : args[1]);
      service.cache_mailbox(mailbox, [](std::size_t done, std::size_t total) {
        print_count_progress("upload-pre-cache", done, total);
      });
      if (std::filesystem::is_directory(local_path)) {
        const auto uploaded_files = service.upload_path(mailbox, local_path, [](std::int64_t current_block,
                                                                                std::int64_t total_blocks,
                                                                                const std::string& file_name) {
          print_block_progress("upload", current_block, total_blocks, file_name);
        });
        service.cache_mailbox(mailbox, [](std::size_t done, std::size_t total) {
          print_count_progress("upload-post-cache", done, total);
        });
        std::cout << "upload complete: files=" << uploaded_files << '\n';
        return 0;
      }

      service.upload_file(mailbox, local_path, [](std::int64_t current_block,
                                                  std::int64_t total_blocks,
                                                  const std::string& file_name) {
        print_block_progress("upload", current_block, total_blocks, file_name);
      });
      service.cache_mailbox(mailbox, [](std::size_t done, std::size_t total) {
        print_count_progress("upload-post-cache", done, total);
      });
      std::cout << "upload complete\n";
      return 0;
    }

    if (command == "delete-uid") {
      if (args.size() != 2 && args.size() != 3) {
        print_usage();
        return 1;
      }
      const auto mailbox =
          resolve_mailbox(config, args.size() == 3 ? std::optional<std::string>(args[1]) : std::nullopt, command);
      service.delete_message_uid(mailbox, std::stoull(args.size() == 3 ? args[2] : args[1]));
      std::cout << "delete complete\n";
      return 0;
    }

    if (command == "download") {
      if (args.size() != 2 && args.size() != 3) {
        print_usage();
        return 1;
      }
      const auto mailbox =
          resolve_mailbox(config, args.size() == 3 ? std::optional<std::string>(args[1]) : std::nullopt, command);
      const auto download_target = args.size() == 3 ? args[2] : args[1];
      const auto output_path = service.download_file(mailbox, download_target, [](std::int64_t current_block,
                                                                                  std::int64_t total_blocks,
                                                                                  const std::string& file_name) {
        print_block_progress("download", current_block, total_blocks, file_name);
      });
      std::cout << "download complete: " << output_path.u8string() << '\n';
      return 0;
    }

    if (command == "export-playlist") {
      if (args.size() > 4) {
        print_usage();
        return 1;
      }

      std::optional<std::string> explicit_mailbox;
      std::string output_arg;
      std::string prefix;
      if (args.size() == 2) {
        if (!config.default_mailbox.empty() && looks_like_json_output(args[1])) {
          output_arg = args[1];
        } else if (!config.default_mailbox.empty() && looks_like_local_path(args[1])) {
          prefix = args[1];
        } else {
          explicit_mailbox = args[1];
        }
      } else if (args.size() == 3) {
        if (!config.default_mailbox.empty() && looks_like_json_output(args[1])) {
          output_arg = args[1];
          prefix = args[2];
        } else {
          explicit_mailbox = args[1];
          output_arg = args[2];
        }
      } else if (args.size() == 4) {
        explicit_mailbox = args[1];
        output_arg = args[2];
        prefix = args[3];
      }

      const auto mailbox = resolve_mailbox(config, explicit_mailbox, command);
      const auto output_path =
          !output_arg.empty()
              ? std::filesystem::u8path(output_arg)
              : std::filesystem::u8path(last_segment(mailbox).empty() ? "playlist.json"
                                                                      : last_segment(mailbox) + "_playlist.json");
      const auto playlist_json = service.export_playlist_json(mailbox, prefix);

      if (output_path.has_parent_path()) {
        std::filesystem::create_directories(output_path.parent_path());
      }
      std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
      if (!output) {
        throw std::runtime_error("failed to open output file: " + output_path.u8string());
      }
      output << playlist_json;
      output.close();
      std::cout << "playlist exported: " << output_path.u8string() << '\n';
      return 0;
    }

    if (command == "serve-http") {
      if (args.size() == 3 && args[1] == "--listen") {
        config.http_listen_addr = args[2];
      } else if (args.size() != 1) {
        print_usage();
        return 1;
      }

      mailfs::application::HttpImapDownloadServer server(config);
      std::cout << "HTTP-to-IMAP server listening on " << config.http_listen_addr << '\n';
      server.serve();
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
