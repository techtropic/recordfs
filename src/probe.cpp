// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Marc Micalizzi
#include "probe.h"

#include "filecache.h"
#include "fsdclient.h"
#include "log.h"
#include "recordspace.h"
#include "wsclient.h"

#include <cstdio>

namespace rfs {

namespace {
FileEntry entry_from_json(const nlohmann::json& f) {
  FileEntry e;
  e.guid = f.value("guid", "");
  e.path = f.value("filename", "");
  e.location = f.value("location", "");
  e.mimetype = f.value("mimetype", "");
  e.size = f.value("size", (uint64_t)0);
  e.modified_on = f.value("modified_on", "");
  e.can_write = f.value("can_write", false);
  return e;
}
}  // namespace

int run_probe(const Options& o) {
  std::string table = o.table.empty() ? "workorders" : o.table;
  std::printf("recordfs probe — %s\n", o.server.c_str());

  WsClient ws(o.server);
  if (!ws.login(o.token)) {
    error("login failed: token invalid, expired, or revoked");
    return 1;
  }
  std::printf("  [1] file-session login        OK\n");

  auto listing = ws.list_objects(table, 0, 25);
  if (listing.contains("error")) {
    error("list_objects: " + listing["error"].get<std::string>());
    return 1;
  }
  auto objects = listing.value("objects", nlohmann::json::array());
  uint64_t total = listing.value("total", (uint64_t)0);
  std::printf("  [2] list_objects %-12s OK (%llu records, showing %zu)\n", table.c_str(),
              (unsigned long long)total, objects.size());
  if (objects.empty()) {
    error("table has no records to probe against");
    return 1;
  }

  // Focus record: --key if given, else the first record with files.
  nlohmann::json files;
  std::string focus_key, focus_display;
  auto try_object = [&](const nlohmann::json& obj) {
    std::string key = obj.value("key", "");
    bool numeric = obj.contains("id");
    auto r = ws.list_files(table, numeric ? "id" : "guid",
                           numeric ? nlohmann::json(obj["id"]) : nlohmann::json(key));
    auto fl = r.value("files", nlohmann::json::array());
    if (fl.empty()) return false;
    files = fl;
    focus_key = key;
    focus_display = obj.value("display", "");
    return true;
  };
  if (!o.key.empty()) {
    nlohmann::json obj;
    obj["key"] = o.key;
    bool numeric = false;
    try {
      obj["id"] = std::stoll(o.key);
      numeric = true;
    } catch (...) {}
    nlohmann::json dd;
    dd["table"] = table;
    dd["key_type"] = numeric ? "id" : "guid";
    dd["key"] = numeric ? obj["id"] : obj["key"];
    obj["display"] = ws.request("get_object_display", std::move(dd)).value("display", "");
    if (!try_object(obj)) {
      error("record " + o.key + " has no visible files");
      return 1;
    }
  } else {
    bool found = false;
    for (const auto& obj : objects)
      if ((found = try_object(obj))) break;
    if (!found) {
      error("none of the first " + std::to_string(objects.size()) +
            " records have visible files (try --key)");
      return 1;
    }
  }
  std::printf("  [3] list_files                OK (%zu entries)\n", files.size());
  std::printf("      \\%s\\%s\\\n", table.c_str(),
              object_dir_name(focus_key, focus_display).c_str());

  FileTree tree;
  {
    std::vector<FileEntry> entries;
    for (const auto& f : files) entries.push_back(entry_from_json(f));
    tree.build(std::move(entries));
  }
  const FileEntry* target = nullptr;
  for (const auto& c : tree.list(""))
    if (!c.is_dir && c.file->location.starts_with("sha256:")) { target = c.file; break; }
  if (!target)
    for (const auto& e : tree.entries())
      if (!e.is_dir && e.location.starts_with("sha256:")) { target = &e; break; }
  if (!target) {
    error("record has no daemon-backed (sha256:) files — is the file service deployed / data migrated?");
    return 1;
  }
  if (o.verbose)
    for (const auto& e : tree.entries())
      std::printf("      - %s (%s, %llu bytes)\n", e.path.c_str(),
                  e.is_dir ? "dir" : e.location.substr(0, 14).c_str(),
                  (unsigned long long)e.size);

  auto grant = ws.get_file_token(target->guid);
  if (!grant.contains("result")) {
    error("get_file_token: " + grant.value("error", "unknown failure"));
    return 1;
  }
  auto res = grant["result"];
  std::string mode = res.value("mode", "");
  auto urls = res.value("urls", std::vector<std::string>{});
  std::string fingerprint = res.value("fingerprint", "");
  std::printf("  [4] get_file_token            OK (mode=%s, %zu url%s)\n", mode.c_str(),
              urls.size(), urls.size() == 1 ? "" : "s");
  if (mode != "direct") {
    error("server minted mode=" + mode +
          " — the direct byte plane is unavailable and protocol v1 file sessions have no proxy "
          "fallback (check the daemon's advertise_urls / port publish)");
    return 1;
  }

  FileCache cache;
  auto dest = cache.path_for(target->location);
  bool was_cached = cache.present(target->location);
  if (was_cached) std::filesystem::remove(dest);  // probe = prove the wire, not the cache
  auto fetch = fetch_blob(urls, fingerprint, target->location, res.value("token", ""), dest);
  if (!fetch.ok) {
    error("fetch: " + fetch.error);
    return 1;
  }
  std::printf("  [5] fetch %-19s OK (%llu bytes in %.0f ms via %s, hash verified%s)\n",
              target->path.c_str(), (unsigned long long)fetch.size, fetch.ms,
              fetch.url_used.c_str(), was_cached ? ", re-fetched" : "");
  std::printf("      cached at %s\n", dest.string().c_str());
  std::printf("probe PASSED\n");
  return 0;
}

}  // namespace rfs
