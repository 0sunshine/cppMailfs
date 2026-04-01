#include "mailfs/core/model/mail_block_metadata.hpp"

#include <sstream>
#include <stdexcept>
#include <vector>

namespace mailfs::core::model {

namespace {

std::vector<std::string> split(const std::string& text, char delimiter) {
  std::vector<std::string> parts;
  std::stringstream stream(text);
  std::string item;
  while (std::getline(stream, item, delimiter)) {
    parts.push_back(item);
  }
  return parts;
}

}  // namespace

nlohmann::json MailBlockMetadata::to_json() const {
  return {
      {"subject", subject},
      {"file_md5", file_md5},
      {"block_md5", block_md5},
      {"file_size", file_size},
      {"block_size", block_size},
      {"create_time", create_time},
      {"owner", owner},
      {"local_path", local_path},
      {"mail_folder", mail_folder},
      {"block_seq", block_seq},
      {"block_count", block_count},
      {"encrypted", encrypted},
  };
}

std::string MailBlockMetadata::to_json_text() const {
  return to_json().dump();
}

MailBlockMetadata MailBlockMetadata::from_json(const nlohmann::json& json_value) {
  MailBlockMetadata metadata;
  metadata.subject = json_value.at("subject").get<std::string>();
  metadata.file_md5 = json_value.at("file_md5").get<std::string>();
  metadata.block_md5 = json_value.at("block_md5").get<std::string>();
  metadata.file_size = json_value.at("file_size").get<std::uint64_t>();
  metadata.block_size = json_value.at("block_size").get<std::uint64_t>();
  metadata.create_time = json_value.at("create_time").get<std::string>();
  metadata.owner = json_value.at("owner").get<std::string>();
  metadata.local_path = json_value.at("local_path").get<std::string>();
  metadata.mail_folder = json_value.at("mail_folder").get<std::string>();
  metadata.block_seq = json_value.value("block_seq", 0);
  metadata.block_count = json_value.value("block_count", 0);
  metadata.encrypted = json_value.value("encrypted", false);

  if ((metadata.block_seq == 0 || metadata.block_count == 0) && !metadata.subject.empty()) {
    const auto subject_info = parse_subject(metadata.subject);
    metadata.block_seq = subject_info.block_seq;
    metadata.block_count = subject_info.block_count;
    metadata.encrypted = metadata.encrypted || subject_info.encrypted;
  }

  return metadata;
}

MailBlockMetadata MailBlockMetadata::from_json_text(const std::string& json_text) {
  return from_json(nlohmann::json::parse(json_text));
}

SubjectInfo MailBlockMetadata::parse_subject(const std::string& subject) {
  const auto parts = split(subject, '/');
  if (parts.size() != 3) {
    throw std::runtime_error("invalid subject format: " + subject);
  }

  const auto seq_parts = split(parts[2], '-');
  if (seq_parts.size() != 2) {
    throw std::runtime_error("invalid subject block format: " + subject);
  }

  SubjectInfo info;
  info.file_name = parts[0];
  info.encrypted = parts[1] == "encrypted";
  info.block_seq = std::stoi(seq_parts[0]);
  info.block_count = std::stoi(seq_parts[1]);
  return info;
}

std::string MailBlockMetadata::make_subject(const std::string& file_name,
                                            bool encrypted,
                                            std::int32_t block_seq,
                                            std::int32_t block_count) {
  std::ostringstream stream;
  stream << file_name << '/' << (encrypted ? "encrypted" : "plain") << '/' << block_seq << '-' << block_count;
  return stream.str();
}

}  // namespace mailfs::core::model
