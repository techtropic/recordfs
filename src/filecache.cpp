// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Marc Micalizzi
#include "filecache.h"

#include <cstdlib>

namespace rfs {

namespace {
std::filesystem::path default_root() {
  char* base = nullptr;
  size_t len = 0;
  std::filesystem::path root;
  if (_dupenv_s(&base, &len, "LOCALAPPDATA") == 0 && base) {
    root = std::filesystem::path(base) / "RecordFS" / "cache";
    free(base);
  } else {
    root = std::filesystem::temp_directory_path() / "RecordFS" / "cache";
  }
  return root;
}
}  // namespace

FileCache::FileCache() : root_(default_root()) {}
FileCache::FileCache(std::filesystem::path root) : root_(std::move(root)) {}

std::filesystem::path FileCache::path_for(const std::string& hash) const {
  std::string hex = hash.starts_with("sha256:") ? hash.substr(7) : hash;
  if (hex.size() < 3) return root_ / "invalid" / hex;
  return root_ / hex.substr(0, 2) / hex;
}

bool FileCache::present(const std::string& hash) const {
  std::error_code ec;
  return std::filesystem::exists(path_for(hash), ec);
}

}  // namespace rfs
