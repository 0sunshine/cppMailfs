#pragma once

#include <string>
#include <string_view>

namespace mailfs::infra::imap {

std::string decode_imap_utf7(std::string_view text);
std::string encode_imap_utf7(std::string_view text);

}  // namespace mailfs::infra::imap
