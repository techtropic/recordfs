// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Marc Micalizzi
#include "recordspace.h"

#include <algorithm>

namespace rfs {

std::string sanitize_component(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s) {
    if (c < 0x20 || c == '<' || c == '>' || c == ':' || c == '"' || c == '/' ||
        c == '\\' || c == '|' || c == '?' || c == '*')
      out.push_back('_');
    else
      out.push_back((char)c);
  }
  while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
  if (out.size() > 120) out.resize(120);
  if (out.empty()) out = "_";
  return out;
}

std::string object_dir_name(const std::string& key, const std::string& display) {
  std::string d = sanitize_component(display);
  if (d == "_" || d.empty() || d == key) return sanitize_component(key);
  return sanitize_component(key) + " - " + d;
}

std::optional<std::string> key_from_dir_name(const std::string& dir) {
  auto sep = dir.find(" - ");
  std::string key = (sep == std::string::npos) ? dir : dir.substr(0, sep);
  if (key.empty()) return std::nullopt;
  return key;
}

namespace {
// "photos/install-1.jpg" -> {"photos", "install-1.jpg"}; tolerates leading
// '/', backslashes, and duplicate separators.
std::vector<std::string> split_path(const std::string& p) {
  std::vector<std::string> parts;
  std::string cur;
  for (char c : p) {
    if (c == '/' || c == '\\') {
      if (!cur.empty()) parts.push_back(std::move(cur));
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) parts.push_back(std::move(cur));
  return parts;
}

std::string join_dir(const std::vector<std::string>& parts, size_t n) {
  std::string out;
  for (size_t i = 0; i < n; ++i) {
    if (i) out.push_back('/');
    out += parts[i];
  }
  return out;
}
}  // namespace

void FileTree::build(std::vector<FileEntry> entries) {
  entries_ = std::move(entries);
  dirs_.clear();
  dirs_[""];  // the root always exists
  for (size_t i = 0; i < entries_.size(); ++i) {
    auto& e = entries_[i];
    e.is_dir = e.is_dir || e.mimetype == "inode/directory";
    auto parts = split_path(e.path);
    if (parts.empty()) continue;
    e.path = join_dir(parts, parts.size());  // canonical form
    // Intermediate components are directories.
    for (size_t d = 0; d + 1 < parts.size(); ++d) {
      dirs_[join_dir(parts, d)].emplace(parts[d], SIZE_MAX);
      dirs_[join_dir(parts, d + 1)];
    }
    std::string parent = join_dir(parts, parts.size() - 1);
    if (e.is_dir) {
      dirs_[parent].emplace(parts.back(), SIZE_MAX);
      dirs_[e.path];
    } else {
      dirs_[parent][parts.back()] = i;  // file wins over implied dir of same name
    }
  }
}

std::vector<FileTree::Child> FileTree::list(const std::string& dir) const {
  std::vector<Child> out;
  auto it = dirs_.find(dir);
  if (it == dirs_.end()) return out;
  out.reserve(it->second.size());
  for (const auto& [name, idx] : it->second) {
    Child c;
    c.name = name;
    c.is_dir = idx == SIZE_MAX;
    c.file = c.is_dir ? nullptr : &entries_[idx];
    out.push_back(std::move(c));
  }
  return out;
}

std::optional<FileTree::Child> FileTree::find(const std::string& path) const {
  auto parts = split_path(path);
  if (parts.empty()) {
    Child root;
    root.is_dir = true;
    return root;
  }
  std::string parent = join_dir(parts, parts.size() - 1);
  auto it = dirs_.find(parent);
  if (it == dirs_.end()) return std::nullopt;
  auto ct = it->second.find(parts.back());
  if (ct == it->second.end()) return std::nullopt;
  Child c;
  c.name = ct->first;
  c.is_dir = ct->second == SIZE_MAX;
  c.file = c.is_dir ? nullptr : &entries_[ct->second];
  return c;
}

}  // namespace rfs
