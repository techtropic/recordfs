// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Marc Micalizzi
#pragma once
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace rfs {

// The mapping between protocol namespace and filesystem names.
//
//   \<table>\<sanitized display>\<attachment path...>
//
// The object directory is the record's DISPLAY string (people navigate by
// what they read in the app). Identity stays the record key: the namespace
// layer keeps a name→key map, and a rare display collision gets a
// deterministic " [<key>]" suffix.

// Replace filesystem-hostile characters, trim trailing dots/spaces, cap length.
std::string sanitize_component(const std::string& s);

// "250089 - Chris Wilson - Lot 22" (falls back to the key when the display
// is empty/unusable).
std::string object_dir_name(const std::string& key, const std::string& display);

struct FileEntry {
  std::string guid;
  std::string path;        // normalized: no leading '/', '/'-separated
  std::string location;    // sha256:<hex> or legacy
  std::string mimetype;
  uint64_t size = 0;
  std::string modified_on; // "YYYY-MM-DD HH:MM:SS" or empty
  bool can_write = false;
  bool is_dir = false;     // explicit inode/directory entry
  bool ephemeral = false;  // server-memory lock/temp file (protocol §4.3)
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
