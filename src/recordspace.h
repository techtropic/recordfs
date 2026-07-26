// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Marc Micalizzi
#pragma once
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace rfs {

// The mapping between protocol namespace and filesystem names.
//
//   \<table>\<key> - <sanitized display>\<attachment path...>
//
// The record key leads the directory name: it is the identity (unique,
// parseable back out); the display part is cosmetic and may change without
// breaking anything that stored a path.

// Replace filesystem-hostile characters, trim trailing dots/spaces, cap length.
std::string sanitize_component(const std::string& s);

// "50049 - 250089 - Chris Wilson - Lot 22"
std::string object_dir_name(const std::string& key, const std::string& display);

// Parse the record key back off an object directory name ("<key> - ..." or
// bare "<key>"). nullopt if the name has no usable key prefix.
std::optional<std::string> key_from_dir_name(const std::string& dir);

struct FileEntry {
  std::string guid;
  std::string path;        // normalized: no leading '/', '/'-separated
  std::string location;    // sha256:<hex> or legacy
  std::string mimetype;
  uint64_t size = 0;
  std::string modified_on; // "YYYY-MM-DD HH:MM:SS" or empty
  bool can_write = false;
  bool is_dir = false;     // explicit inode/directory entry
};

// A record's attachment listing as a directory tree. Directories exist both
// explicitly (inode/directory entries) and implicitly (path components).
class FileTree {
public:
  struct Child {
    std::string name;
    bool is_dir = false;
    const FileEntry* file = nullptr;  // null for directories
  };

  void build(std::vector<FileEntry> entries);

  // Children of a directory ("" = the record's root). Sorted by name.
  std::vector<Child> list(const std::string& dir) const;

  // Look up one path ("photos/install-1.jpg"). nullopt = doesn't exist.
  std::optional<Child> find(const std::string& path) const;

  const std::vector<FileEntry>& entries() const { return entries_; }

private:
  std::vector<FileEntry> entries_;
  // dir path -> (child name -> file index or SIZE_MAX for directory)
  std::map<std::string, std::map<std::string, size_t>> dirs_;
};

}  // namespace rfs
