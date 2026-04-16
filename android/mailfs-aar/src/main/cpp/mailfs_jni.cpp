#include <jni.h>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include "mailfs/application/http_imap_download_server.hpp"
#include "mailfs/application/mailfs_service.hpp"
#include "mailfs/core/model/app_config.hpp"
#include "mailfs/infra/imap/imap_client.hpp"
#include "mailfs/infra/logging/logger.hpp"
#include "mailfs/infra/storage/sqlite_cache_repository.hpp"

#include <nlohmann/json.hpp>

namespace {

struct ServerState {
  std::mutex mutex;
  std::unique_ptr<mailfs::application::HttpImapDownloadServer> server;
  std::thread worker;
  bool running = false;
  std::string last_error;
};

ServerState& state() {
  static ServerState value;
  return value;
}

std::string java_string(JNIEnv* env, jstring value) {
  if (value == nullptr) {
    return {};
  }
  const char* chars = env->GetStringUTFChars(value, nullptr);
  if (chars == nullptr) {
    return {};
  }
  std::string text(chars);
  env->ReleaseStringUTFChars(value, chars);
  return text;
}

void set_last_error(std::string message) {
  auto& current = state();
  std::lock_guard<std::mutex> lock(current.mutex);
  current.last_error = std::move(message);
}

void join_old_worker_if_needed() {
  std::thread old_worker;
  {
    auto& current = state();
    std::lock_guard<std::mutex> lock(current.mutex);
    if (current.running) {
      return;
    }
    if (current.worker.joinable()) {
      old_worker = std::move(current.worker);
    }
  }

  if (old_worker.joinable()) {
    old_worker.join();
  }

  auto& current = state();
  std::lock_guard<std::mutex> lock(current.mutex);
  if (!current.running) {
    current.server.reset();
  }
}

mailfs::core::model::AppConfig config_from_json(const nlohmann::json& json) {
  mailfs::core::model::AppConfig config;
  config.imap_host = json.value("imapHost", config.imap_host);
  config.imap_port = static_cast<std::uint16_t>(std::max(1, json.value("imapPort", static_cast<int>(config.imap_port))));
  config.credential_file = json.value("credentialFile", config.credential_file);
  config.ca_cert_file = json.value("caCertFile", config.ca_cert_file);
  config.allow_insecure_tls = json.value("allowInsecureTls", config.allow_insecure_tls);
  config.log_level = json.value("logLevel", config.log_level);
  config.log_file = json.value("logFile", config.log_file);
  config.database_path = json.value("databasePath", config.database_path);
  config.download_dir = json.value("downloadDir", config.download_dir);
  config.http_listen_addr = json.value("listenAddr", config.http_listen_addr);
  config.http_copy_addr = json.value("copyAddr", config.http_copy_addr);
  config.default_mailbox = json.value("defaultMailbox", config.default_mailbox);
  config.email_name = json.value("emailName", config.email_name);
  config.owner_name = json.value("ownerName", config.owner_name);
  config.default_block_size = static_cast<std::size_t>(
      std::max<std::int64_t>(1, json.value("defaultBlockSize", static_cast<std::int64_t>(config.default_block_size))));
  config.cache_fetch_batch_size = static_cast<std::size_t>(
      std::max(1, json.value("cacheFetchBatchSize", static_cast<int>(config.cache_fetch_batch_size))));
  return config;
}

void configure_logger(const mailfs::core::model::AppConfig& config) {
  mailfs::infra::logging::Logger::instance().configure({
      mailfs::infra::logging::parse_log_level(config.log_level),
      std::filesystem::u8path(config.log_file),
      true,
      config.log_max_file_size,
      config.log_max_files,
  });
}

std::string format_count_progress(std::size_t done, std::size_t total) {
  std::ostringstream text;
  text << "cache progress: " << done << '/' << total;
  if (total > 0) {
    text << " (" << std::fixed << std::setprecision(1)
         << (static_cast<double>(done) * 100.0 / static_cast<double>(total)) << "%)";
  }
  return text.str();
}

struct ProgressReporter {
  JNIEnv* env = nullptr;
  jobject callback = nullptr;
  jmethodID method = nullptr;

  void emit(const char* action, std::size_t done, std::size_t total, const std::string& message) const {
    if (env == nullptr || callback == nullptr || method == nullptr) {
      return;
    }

    jstring action_text = env->NewStringUTF(action);
    jstring message_text = env->NewStringUTF(message.c_str());
    if (action_text == nullptr || message_text == nullptr) {
      if (action_text != nullptr) {
        env->DeleteLocalRef(action_text);
      }
      if (message_text != nullptr) {
        env->DeleteLocalRef(message_text);
      }
      return;
    }

    env->CallVoidMethod(callback,
                        method,
                        action_text,
                        static_cast<jlong>(done),
                        static_cast<jlong>(total),
                        message_text);
    env->DeleteLocalRef(action_text);
    env->DeleteLocalRef(message_text);
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
    }
  }
};

ProgressReporter progress_reporter_from_callback(JNIEnv* env, jobject callback) {
  if (callback == nullptr) {
    return {};
  }

  jclass callback_class = env->GetObjectClass(callback);
  if (callback_class == nullptr) {
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
    }
    return {};
  }

  jmethodID method =
      env->GetMethodID(callback_class, "onProgress", "(Ljava/lang/String;JJLjava/lang/String;)V");
  env->DeleteLocalRef(callback_class);
  if (method == nullptr) {
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
    }
    return {};
  }

  return ProgressReporter{env, callback, method};
}

std::string resolve_mailbox(const mailfs::core::model::AppConfig& config,
                            const std::string& explicit_mailbox,
                            const std::string& command) {
  if (!explicit_mailbox.empty()) {
    return explicit_mailbox;
  }
  if (!config.default_mailbox.empty()) {
    return config.default_mailbox;
  }
  throw std::runtime_error("mailbox is required for " + command + " because default mailbox is not configured");
}

std::string run_mailfs_command(const nlohmann::json& request, const ProgressReporter& progress_reporter = {}) {
  const auto config = config_from_json(request.at("config"));
  configure_logger(config);

  mailfs::infra::storage::SQLiteCacheRepository repository(std::filesystem::u8path(config.database_path));
  mailfs::infra::imap::ImapClient client;
  mailfs::application::MailfsService service(config, client, repository);
  struct DisconnectGuard {
    mailfs::application::MailfsService& service;
    ~DisconnectGuard() {
      service.disconnect();
    }
  } guard{service};

  const auto command = request.value("command", std::string());
  const auto mailbox_arg = request.value("mailbox", std::string());
  const auto path_arg = request.value("path", std::string());
  const auto output_path_arg = request.value("outputPath", std::string());
  const auto uid = request.value("uid", static_cast<std::uint64_t>(0));
  std::ostringstream output;

  if (command == "list-mailboxes") {
    for (const auto& mailbox : service.list_mailboxes()) {
      output << mailbox << '\n';
    }
    return output.str();
  }

  if (command == "cache-mailbox") {
    const auto mailbox = resolve_mailbox(config, mailbox_arg, command);
    std::size_t last_done = static_cast<std::size_t>(-1);
    std::size_t last_total = static_cast<std::size_t>(-1);
    const auto count = service.cache_mailbox(mailbox, [&](std::size_t done, std::size_t total) {
      if (done == last_done && total == last_total) {
        return;
      }
      last_done = done;
      last_total = total;
      const auto progress_text = format_count_progress(done, total);
      output << progress_text << '\n';
      progress_reporter.emit("cache-mailbox", done, total, progress_text);
    });
    output << "cached messages: " << count << '\n';
    return output.str();
  }

  if (command == "list-cache") {
    const auto mailbox = resolve_mailbox(config, mailbox_arg, command);
    for (const auto& file : service.list_cached_files(mailbox)) {
      output << file.local_path << " blocks=" << file.block_count << " size=" << file.file_size << '\n';
    }
    return output.str();
  }

  if (command == "check-integrity") {
    const auto mailbox = resolve_mailbox(config, mailbox_arg, command);
    const auto results = service.check_cached_integrity(mailbox, path_arg);
    std::size_t ok_count = 0;
    std::size_t broken_count = 0;
    for (const auto& result : results) {
      if (result.ok) {
        ++ok_count;
        continue;
      }
      ++broken_count;
      output << "BROKEN " << result.file.local_path << " cached=" << result.cached_blocks
             << " expected=" << result.expected_blocks << '\n';
    }
    output << "integrity ok=" << ok_count << " broken=" << broken_count << " total=" << results.size() << '\n';
    return output.str();
  }

  if (command == "dedup-mailbox") {
    const auto mailbox = resolve_mailbox(config, mailbox_arg, command);
    const auto results = service.deduplicate_mailbox(mailbox, path_arg);
    std::size_t deleted_uid_count = 0;
    for (const auto& result : results) {
      deleted_uid_count += result.deleted_uids.size();
      output << "DEDUP " << result.local_path << " keep=" << result.kept_uids.size()
             << " delete=" << result.deleted_uids.size() << '\n';
    }
    output << "dedup complete: files=" << results.size() << " deleted_uids=" << deleted_uid_count << '\n';
    return output.str();
  }

  if (command == "delete-uid") {
    if (uid == 0) {
      throw std::runtime_error("uid is required for delete-uid");
    }
    const auto mailbox = resolve_mailbox(config, mailbox_arg, command);
    service.delete_message_uid(mailbox, uid);
    return "delete complete\n";
  }

  if (command == "upload") {
    if (path_arg.empty()) {
      throw std::runtime_error("path is required for upload");
    }
    const auto mailbox = resolve_mailbox(config, mailbox_arg, command);
    const auto local_path = std::filesystem::u8path(path_arg);
    service.cache_mailbox(mailbox);
    if (std::filesystem::is_directory(local_path)) {
      const auto uploaded_files = service.upload_path(mailbox, local_path);
      service.cache_mailbox(mailbox);
      output << "upload complete: files=" << uploaded_files << '\n';
      return output.str();
    }
    service.upload_file(mailbox, local_path);
    service.cache_mailbox(mailbox);
    return "upload complete\n";
  }

  if (command == "download") {
    if (path_arg.empty()) {
      throw std::runtime_error("path is required for download");
    }
    const auto mailbox = resolve_mailbox(config, mailbox_arg, command);
    const auto output_path = service.download_file(mailbox, path_arg);
    output << "download complete: " << output_path.u8string() << '\n';
    return output.str();
  }

  if (command == "export-playlist") {
    const auto mailbox = resolve_mailbox(config, mailbox_arg, command);
    const auto playlist_json = service.export_playlist_json(mailbox, path_arg);
    if (output_path_arg.empty()) {
      return playlist_json + "\n";
    }

    const auto output_path = std::filesystem::u8path(output_path_arg);
    if (output_path.has_parent_path()) {
      std::filesystem::create_directories(output_path.parent_path());
    }
    std::ofstream file(output_path, std::ios::binary | std::ios::trunc);
    if (!file) {
      throw std::runtime_error("failed to open output file: " + output_path.u8string());
    }
    file << playlist_json;
    output << "playlist exported: " << output_path.u8string() << '\n';
    return output.str();
  }

  throw std::runtime_error("unsupported command: " + command);
}

}  // namespace

extern "C" JNIEXPORT jboolean JNICALL
Java_com_mailfs_android_MailfsNative_start(JNIEnv* env,
                                           jclass,
                                           jstring imap_host,
                                           jint imap_port,
                                           jstring credential_file,
                                           jstring ca_cert_file,
                                           jboolean allow_insecure_tls,
                                           jstring log_level,
                                           jstring log_file,
                                           jstring database_path,
                                           jstring download_dir,
                                           jstring listen_addr,
                                           jstring copy_addr,
                                           jstring default_mailbox,
                                           jstring email_name,
                                           jstring owner_name,
                                           jlong default_block_size,
                                           jint cache_fetch_batch_size) {
  try {
    {
      auto& current = state();
      std::lock_guard<std::mutex> lock(current.mutex);
      if (current.running) {
        return JNI_TRUE;
      }
    }

    join_old_worker_if_needed();

    mailfs::core::model::AppConfig config;
    config.imap_host = java_string(env, imap_host);
    config.imap_port = static_cast<std::uint16_t>(std::max<jint>(1, imap_port));
    config.credential_file = java_string(env, credential_file);
    config.ca_cert_file = java_string(env, ca_cert_file);
    config.allow_insecure_tls = allow_insecure_tls == JNI_TRUE;
    config.log_level = java_string(env, log_level);
    config.log_file = java_string(env, log_file);
    config.database_path = java_string(env, database_path);
    config.download_dir = java_string(env, download_dir);
    config.http_listen_addr = java_string(env, listen_addr);
    config.http_copy_addr = java_string(env, copy_addr);
    config.default_mailbox = java_string(env, default_mailbox);
    config.email_name = java_string(env, email_name);
    config.owner_name = java_string(env, owner_name);
    config.default_block_size = static_cast<std::size_t>(std::max<jlong>(1, default_block_size));
    config.cache_fetch_batch_size = static_cast<std::size_t>(std::max<jint>(1, cache_fetch_batch_size));

    mailfs::infra::logging::Logger::instance().configure({
        mailfs::infra::logging::parse_log_level(config.log_level),
        std::filesystem::u8path(config.log_file),
        true,
        config.log_max_file_size,
        config.log_max_files,
    });

    auto server = std::make_unique<mailfs::application::HttpImapDownloadServer>(config);
    auto* raw_server = server.get();

    {
      auto& current = state();
      std::lock_guard<std::mutex> lock(current.mutex);
      current.server = std::move(server);
      current.last_error.clear();
      current.running = true;
    }

    std::thread worker([raw_server]() {
      try {
        raw_server->serve();
      } catch (const std::exception& ex) {
        set_last_error(ex.what());
      } catch (...) {
        set_last_error("unknown native server error");
      }

      auto& current = state();
      std::lock_guard<std::mutex> lock(current.mutex);
      current.running = false;
    });

    auto& current = state();
    std::lock_guard<std::mutex> lock(current.mutex);
    current.worker = std::move(worker);
    return JNI_TRUE;
  } catch (const std::exception& ex) {
    set_last_error(ex.what());
    return JNI_FALSE;
  } catch (...) {
    set_last_error("unknown native startup error");
    return JNI_FALSE;
  }
}

extern "C" JNIEXPORT void JNICALL
Java_com_mailfs_android_MailfsNative_stop(JNIEnv*, jclass) {
  std::thread worker;
  {
    auto& current = state();
    std::lock_guard<std::mutex> lock(current.mutex);
    if (current.server) {
      current.server->stop();
    }
    if (current.worker.joinable()) {
      worker = std::move(current.worker);
    }
  }

  if (worker.joinable()) {
    worker.join();
  }

  auto& current = state();
  std::lock_guard<std::mutex> lock(current.mutex);
  current.server.reset();
  current.running = false;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_mailfs_android_MailfsNative_isRunning(JNIEnv*, jclass) {
  auto& current = state();
  std::lock_guard<std::mutex> lock(current.mutex);
  return current.running ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_mailfs_android_MailfsNative_lastError(JNIEnv* env, jclass) {
  auto& current = state();
  std::lock_guard<std::mutex> lock(current.mutex);
  return env->NewStringUTF(current.last_error.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_mailfs_android_MailfsNative_runCommand(JNIEnv* env, jclass, jstring request_json) {
  try {
    const auto request_text = java_string(env, request_json);
    const auto request = nlohmann::json::parse(request_text);
    const auto result = run_mailfs_command(request);
    set_last_error({});
    return env->NewStringUTF(result.c_str());
  } catch (const std::exception& ex) {
    const auto error = std::string("error: ") + ex.what() + "\n";
    set_last_error(ex.what());
    return env->NewStringUTF(error.c_str());
  } catch (...) {
    const std::string error = "error: unknown native command error\n";
    set_last_error("unknown native command error");
    return env->NewStringUTF(error.c_str());
  }
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_mailfs_android_MailfsNative_runCommandWithProgress(JNIEnv* env,
                                                            jclass,
                                                            jstring request_json,
                                                            jobject progress_callback) {
  try {
    const auto request_text = java_string(env, request_json);
    const auto request = nlohmann::json::parse(request_text);
    const auto progress_reporter = progress_reporter_from_callback(env, progress_callback);
    const auto result = run_mailfs_command(request, progress_reporter);
    set_last_error({});
    return env->NewStringUTF(result.c_str());
  } catch (const std::exception& ex) {
    const auto error = std::string("error: ") + ex.what() + "\n";
    set_last_error(ex.what());
    return env->NewStringUTF(error.c_str());
  } catch (...) {
    const std::string error = "error: unknown native command error\n";
    set_last_error("unknown native command error");
    return env->NewStringUTF(error.c_str());
  }
}
