// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Marc Micalizzi
#pragma once
#include <filesystem>
#include <string>

namespace rfs {

// Local content-addressed cache: %LOCALAPPDATA%\RecordFS\cache\<ab>\<hex>.
// Content is immutable (identified by its hash), so presence == validity —
// there is no invalidation, only eventual size-based eviction (future work).
class FileCache {
public:
  FileCache();  // default root under %LOCALAPPDATA%
  explicit FileCache(std::filesystem::path root);

  const std::filesystem::path& root() const { return root_; }

  // Where this hash lives/would live. hash = "sha256:<hex>".
  std::filesystem::path path_for(const std::string& hash) const;
  bool present(const std::string& hash) const;

private:
  std::filesystem::path root_;
};

}  // namespace rfs
