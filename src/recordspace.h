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

// Case folding for names on the drive. The volume is case-insensitive (as
// Windows applications expect), so "PLAN.DWG" must find "plan.dwg" -- otherwise
// an app that re-types a path gets "not found", and a save through it creates
// a second attachment beside the first. ASCII only, matching every other name
// comparison in the mount and the server's lease keys.
std::string fold_name(std::string s);

// A record's attachment listing as a directory tree. Directories exist both
// explicitly (inode/directory entries) and implicitly (path components).
//
// Lookups are case-insensitive; names come back in their STORED case. When
// stored paths disagree on a directory's case ("Photos/a.jpg", "photos/b.jpg")
// they are one directory, shown in the first spelling seen.
class FileTree {
public:
  struct Child {
    std::string name;                 // stored case
    bool is_dir = false;
    const FileEntry* file = nullptr;  // null for directories
  };

  void build(std::vector<FileEntry> entries);

  // Children of a directory ("" = the record's root), in any case.
  // Sorted case-insensitively.
  std::vector<Child> list(const std::string& dir) const;

  // Look up one path ("photos/install-1.jpg"), in any case. nullopt = doesn't
  // exist.
  std::optional<Child> find(const std::string& path) const;

  // `path` with every component that already exists spelled the way it is
  // stored; components that do not exist keep the caller's spelling. New
  // attachments are registered under this, so creating "PHOTOS/new.jpg"
  // lands in the existing "photos" folder rather than a case-variant twin.
  std::string canonical(const std::string& path) const;

  const std::vector<FileEntry>& entries() const { return entries_; }

private:
  struct Slot {
    std::string name;  // stored case
    size_t idx;        // file index, or SIZE_MAX for a directory
  };
  std::vector<FileEntry> entries_;
  // folded dir path -> (folded child name -> slot)
  std::map<std::string, std::map<std::string, Slot>> dirs_;
};

}  // namespace rfs
