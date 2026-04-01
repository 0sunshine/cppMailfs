#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace mailfs::core::model {

struct CachedBlockRecord {
  std::int32_t block_seq = 0;
  std::uint64_t uid = 0;
  std::string block_md5;
  std::uint64_t block_size = 0;
};

struct CachedFileRecord {
  std::int64_t file_id = 0;
  std::string mail_folder;
  std::string local_path;
  std::int32_t block_count = 0;
  std::string file_md5;
  std::uint64_t file_size = 0;
  std::vector<CachedBlockRecord> blocks;

  void sort_blocks() {
    std::sort(blocks.begin(), blocks.end(), [](const CachedBlockRecord& lhs, const CachedBlockRecord& rhs) {
      return lhs.block_seq < rhs.block_seq;
    });
  }
};

}  // namespace mailfs::core::model
