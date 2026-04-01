#pragma once

#include <string>

namespace mailfs::core::security {

std::string encrypt_string(const std::string& value);
std::string decrypt_string(const std::string& value);
bool should_encrypt_mailbox(const std::string& mailbox);

}  // namespace mailfs::core::security
