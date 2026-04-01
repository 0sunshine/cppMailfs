#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

namespace mailfs::core::model {

struct SubjectInfo {
  std::string file_name;
  bool encrypted = false;
  std::int32_t block_seq = 0;
  std::int32_t block_count = 0;
};

struct MailBlockMetadata {
  std::string subject;
  std::string file_md5;
  std::string block_md5;
  std::uint64_t file_size = 0;
  std::uint64_t block_size = 0;
  std::string create_time;
  std::string owner;
  std::string local_path;
  std::string mail_folder;
  std::int32_t block_seq = 0;
  std::int32_t block_count = 0;
  bool encrypted = false;

  [[nodiscard]] nlohmann::json to_json() const;
  [[nodiscard]] std::string to_json_text() const;

  static MailBlockMetadata from_json(const nlohmann::json& json_value);
  static MailBlockMetadata from_json_text(const std::string& json_text);
  static MailBlockMetadata from_legacy_text(const std::string& text);
  static MailBlockMetadata from_serialized_text(const std::string& text);
  static SubjectInfo parse_subject(const std::string& subject);
  static std::string make_subject(const std::string& file_name,
                                  bool encrypted,
                                  std::int32_t block_seq,
                                  std::int32_t block_count);
};

}  // namespace mailfs::core::model
