// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Marc Micalizzi
//
// The WinFsp adapter: presents the record namespace as a drive.
//
//   S:\<table>\<record display>\<attachment path...>
//
// Browse + open (hydrate-on-open, reads served from the local
// content-addressed cache), plus the ephemeral plane's writable lock/temp
// files. Durable attachments are read-only per file until the write-back
// engine lands behind this same namespace service.
//
// Directory names are display strings, but IDENTITY IS ALWAYS THE RECORD KEY:
// an open handle carries its record by key, and a path whose display has
// since changed still resolves through the former-names grace map.
//
// NOTE: compiled only when the WinFsp SDK is present (RECORDFS_HAVE_WINFSP).
#ifdef RECORDFS_HAVE_WINFSP

#include "config.h"
#include "filecache.h"
#include "fsdclient.h"
#include "log.h"
#include "recordspace.h"
#include "wsclient.h"

#include <windows.h>
#include <winternl.h>
// Some SDK header sets declare NTSTATUS but not PNTSTATUS, which winfsp.h's
// directory-buffer prototypes use. A duplicate identical typedef is legal.
typedef NTSTATUS* PNTSTATUS;
#include <winfsp/winfsp.h>

#include <sddl.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <set>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <atomic>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace rfs {

namespace {

// ---------------------------------------------------------------------------
// small conversions

std::string narrow(const wchar_t* w) {
  if (!w || !*w) return {};
  int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  std::string s(n > 0 ? n - 1 : 0, '\0');
  if (n > 1) ::WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}

std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w(n, L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
  return w;
}

std::string lower_ascii(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return (char)std::tolower(c); });
  return s;
}

uint64_t now_filetime() {
  FILETIME ft;
  ::GetSystemTimeAsFileTime(&ft);
  return ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

// "YYYY-MM-DD HH:MM:SS" (server-local) -> FILETIME as uint64; fallback ft0.
uint64_t parse_timestamp(const std::string& s, uint64_t fallback) {
  SYSTEMTIME st{};
  if (6 != sscanf_s(s.c_str(), "%4hu-%2hu-%2hu %2hu:%2hu:%2hu", &st.wYear, &st.wMonth,
                    &st.wDay, &st.wHour, &st.wMinute, &st.wSecond))
    return fallback;
  SYSTEMTIME utc{};
  if (!::TzSpecificLocalTimeToSystemTime(nullptr, &st, &utc)) utc = st;
  FILETIME ft;
  if (!::SystemTimeToFileTime(&utc, &ft)) return fallback;
  return ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

// ---------------------------------------------------------------------------
// server-backed namespace with TTL caches (dispatcher threads -> mutex)

// How long a renamed record keeps answering to its previous directory name.
// Long enough to cover an edit-and-save session that straddles a rename;
// short enough that a name freed by a rename becomes genuinely free soon.
constexpr auto kFormerNameGrace = std::chrono::minutes(10);
// The set of tables holding attachments changes rarely; the root listing asks
// often. Long enough to be cheap, short enough that a newly attached-to table
// shows up without a remount.
constexpr auto kTableListTtl = std::chrono::seconds(60);
// How often the notify pump re-reads an active record, and how long a record
// stays active after it was last touched. A peer's save has to be noticed by
// polling: attachments have no change stream, so this interval IS the
// staleness bound an application sees.
constexpr auto kNotifyPollInterval = std::chrono::seconds(5);
constexpr auto kActiveRecordLinger = std::chrono::minutes(2);
// A blob the daemon does not have will not appear on its own; a daemon that
// was briefly unreachable will. Remember the former far longer.
constexpr auto kMissingBlobTtl = std::chrono::minutes(10);
constexpr auto kTransientFailureTtl = std::chrono::seconds(20);
constexpr size_t kMaxFormerNames = 4096;

struct ObjectEntry {
  std::string key;       // record key (identity)
  std::string dir_name;  // the record's display string (see object_dir_name)
  bool numeric = false;
  long long id = 0;
};

// Set once the filesystem exists; the namespace layer reports peer-side
// changes through it. Declared here because NamespaceService is defined
// before the volume it notifies.
void notify_change(const std::string& table, const std::string& dir_name,
                   const std::string& inner, UINT32 action);

class NamespaceService {
public:
  NamespaceService(WsClient& ws, FileCache& cache)
      : ws_(ws), cache_(cache), mount_time_(now_filetime()) {}

  uint64_t mount_time() const { return mount_time_; }

  // The root listing. Tables are DISCOVERED from the server -- any table
  // holding at least one attachment that this user may read -- so a table
  // appears by itself once its first attachment is added, and no fixed list
  // has to be maintained anywhere.
  std::vector<std::string> tables() {
    std::lock_guard lock(tables_mutex_);
    auto now = std::chrono::steady_clock::now();
    if (tables_.empty() || fetched_tables_ + kTableListTtl < now) {
      try {
        auto r = ws_.list_tables();
        std::vector<std::string> found;
        std::map<std::string, bool> writable;
        for (const auto& t : r.value("tables", nlohmann::json::array())) {
          std::string name = t.value("table", "");
          if (name.empty()) continue;
          found.push_back(name);
          // Absent means an older server that does not report it; assume
          // writable and let the server refuse -- never silently read-only.
          writable[lower_ascii(name)] = t.value("can_write", true);
        }
        if (!writable.empty()) writable_ = std::move(writable);
        // Keep the previous list on an empty/failed answer rather than making
        // the whole drive look empty.
        if (!found.empty() || !tables_.empty()) {
          if (!found.empty()) tables_ = std::move(found);
          fetched_tables_ = now;
        }
      } catch (const std::exception& e) {
        warn(std::string("list_tables: ") + e.what());
      }
    }
    return tables_;
  }

  bool is_table(const std::string& name) {
    for (const auto& t : tables())
      if (lower_ascii(t) == lower_ascii(name)) return true;
    return false;
  }

  // Whether this user may write ANY attachment in the table (the table-level
  // write mask). Per-attachment masks still apply on top, via can_write.
  bool table_writable(const std::string& name) {
    tables();  // refresh if stale
    std::lock_guard lock(tables_mutex_);
    auto it = writable_.find(lower_ascii(name));
    return it == writable_.end() ? true : it->second;
  }

  // Table-dir listing (one list_objects, TTL-cached).
  std::vector<ObjectEntry> objects(const std::string& table) {
    std::lock_guard lock(mutex_);
    auto& c = objects_[lower_ascii(table)];
    auto now = std::chrono::steady_clock::now();
    if (c.fetched + std::chrono::seconds(30) < now || c.entries.empty()) {
      auto r = ws_.list_objects(table);
      std::vector<std::pair<std::string, std::string>> prev;  // lower(name), key
      prev.reserve(c.by_name.size());
      for (const auto& [n, idx] : c.by_name) prev.emplace_back(n, c.entries[idx].key);
      c.entries.clear();
      c.by_name.clear();
      for (const auto& o : r.value("objects", nlohmann::json::array())) {
        ObjectEntry e;
        e.key = o.value("key", "");
        if (e.key.empty()) continue;
        e.numeric = o.contains("id");
        if (e.numeric) e.id = o["id"].get<long long>();
        // Dir name = the display string alone (Marc: people navigate by what
        // the app shows). Identity is still the key: a display collision gets
        // a deterministic " [<key>]" suffix so both records stay reachable.
        e.dir_name = object_dir_name(e.key, o.value("display", ""));
        if (c.by_name.count(lower_ascii(e.dir_name)))
          e.dir_name += " [" + sanitize_component(e.key) + "]";
        c.by_name[lower_ascii(e.dir_name)] = c.entries.size();
        c.entries.push_back(std::move(e));
      }
      // Retire names that no longer resolve to the same record. A name now
      // held by a DIFFERENT record is still retired here, but live names are
      // matched first in find_object, so the current holder always wins --
      // a reused display can never be hijacked by this map.
      for (const auto& [n, key] : prev) {
        auto it = c.by_name.find(n);
        if (it != c.by_name.end() && c.entries[it->second].key == key) continue;
        c.former[n] = FormerName{key, now};
      }
      for (auto it = c.former.begin(); it != c.former.end();) {
        if (now - it->second.retired > kFormerNameGrace) it = c.former.erase(it);
        else ++it;
      }
      // Backstop against pathological rename churn; the age prune is the
      // real bound.
      if (c.former.size() > kMaxFormerNames) c.former.clear();
      c.fetched = now;
    }
    return c.entries;
  }

  std::optional<ObjectEntry> find_object(const std::string& table, const std::string& dir_name) {
    objects(table);  // refresh if stale
    std::lock_guard lock(mutex_);
    auto& c = objects_[lower_ascii(table)];
    auto it = c.by_name.find(lower_ascii(dir_name));
    if (it != c.by_name.end()) return c.entries[it->second];
    // A collision-suffixed name ("... [<key>]") stays resolvable by key even
    // if the display changed between listings.
    auto lb = dir_name.rfind(" [");
    if (lb != std::string::npos && dir_name.back() == ']') {
      std::string key = dir_name.substr(lb + 2, dir_name.size() - lb - 3);
      for (const auto& e : c.entries)
        if (e.key == key) return e;
    }
    // Finally: a name this record answered to before a recent rename.
    auto ft = c.former.find(lower_ascii(dir_name));
    if (ft != c.former.end()) {
      for (const auto& e : c.entries)
        if (e.key == ft->second.key) return e;
    }
    return std::nullopt;
  }

  // A record's file tree (TTL-cached per record). Touching a record marks it
  // ACTIVE, so the notify pump keeps watching it for peer changes.
  std::shared_ptr<const FileTree> files(const std::string& table, const ObjectEntry& obj) {
    mark_active(table, obj);
    return files_locked(table, obj, false);
  }

  // force=true refetches regardless of TTL (the notify pump).
  std::shared_ptr<const FileTree> files_locked(const std::string& table, const ObjectEntry& obj,
                                               bool force) {
    std::string ck = lower_ascii(table) + "\x1f" + obj.key;
    std::lock_guard lock(mutex_);
    auto& c = trees_[ck];
    auto now = std::chrono::steady_clock::now();
    if (!c.tree || force || c.fetched + std::chrono::seconds(10) < now) {
      auto r = ws_.list_files(table, obj.numeric ? "id" : "guid",
                              obj.numeric ? nlohmann::json(obj.id) : nlohmann::json(obj.key));
      std::vector<FileEntry> entries;
      for (const auto& f : r.value("files", nlohmann::json::array())) {
        FileEntry e;
        e.guid = f.value("guid", "");
        e.path = f.value("filename", "");
        e.location = f.value("location", "");
        e.mimetype = f.value("mimetype", "");
        e.size = f.value("size", (uint64_t)0);
        e.modified_on = f.value("modified_on", "");
        e.can_write = f.value("can_write", false);
        e.ephemeral = f.value("ephemeral", false);
        entries.push_back(std::move(e));
      }
      auto tree = std::make_shared<FileTree>();
      tree->build(std::move(entries));
      // Diff against what we previously believed, so a change made by SOMEONE
      // ELSE becomes a filesystem change notification. Changes made through
      // this machine are notified by the WinFsp driver itself, so those are
      // filtered out (see note_local_write) to avoid duplicate events.
      if (c.tree) emit_tree_diff(table, obj, *c.tree, *tree);
      c.tree = std::move(tree);
      c.fetched = now;
    }
    return c.tree;
  }

  // Turn "what changed since we last looked" into filesystem notifications.
  // Only content-bearing differences count: a file that appeared, vanished,
  // or whose bytes moved to a different hash.
  void emit_tree_diff(const std::string& table, const ObjectEntry& obj,
                      const FileTree& before, const FileTree& after) {
    std::map<std::string, const FileEntry*> old_by_path, new_by_path;
    for (const auto& e : before.entries())
      if (!e.is_dir) old_by_path[lower_ascii(e.path)] = &e;
    for (const auto& e : after.entries())
      if (!e.is_dir) new_by_path[lower_ascii(e.path)] = &e;

    for (const auto& [k, e] : new_by_path) {
      auto it = old_by_path.find(k);
      if (it == old_by_path.end()) {
        if (!was_local_write(table, obj.key, e->path, e->location))
          notify_change(table, obj.dir_name, e->path, FILE_ACTION_ADDED);
      } else if (it->second->location != e->location) {
        if (!was_local_write(table, obj.key, e->path, e->location))
          notify_change(table, obj.dir_name, e->path, FILE_ACTION_MODIFIED);
      }
    }
    for (const auto& [k, e] : old_by_path) {
      if (!new_by_path.count(k))
        notify_change(table, obj.dir_name, e->path, FILE_ACTION_REMOVED);
    }
  }

  // Remember content this machine just wrote, so the pump does not report our
  // own save back to us as a peer change.
  void note_local_write(const std::string& table, const ObjectEntry& obj,
                        const std::string& inner, const std::string& location) {
    std::lock_guard lock(local_writes_mutex_);
    if (local_writes_.size() > 512) local_writes_.clear();
    local_writes_[lower_ascii(table) + "\x1f" + obj.key + "\x1f" + lower_ascii(inner)] = location;
  }
  bool was_local_write(const std::string& table, const std::string& key,
                       const std::string& inner, const std::string& location) {
    std::lock_guard lock(local_writes_mutex_);
    auto k = lower_ascii(table) + "\x1f" + key + "\x1f" + lower_ascii(inner);
    auto it = local_writes_.find(k);
    if (it == local_writes_.end() || it->second != location) return false;
    local_writes_.erase(it);   // one-shot: only the write we just made
    return true;
  }

  void mark_active(const std::string& table, const ObjectEntry& obj) {
    std::lock_guard lock(active_mutex_);
    active_[lower_ascii(table) + "\x1f" + obj.key] = ActiveRecord{
        table, obj, std::chrono::steady_clock::now()};
  }

  // Poll every active record and turn peer-side changes into notifications.
  // Runs on its own thread: an application holding a drawing open may never
  // touch the directory again, so nothing else would drive a refresh.
  void run_notify_pump(HANDLE stop) {
    for (;;) {
      if (::WaitForSingleObject(stop, (DWORD)std::chrono::duration_cast<std::chrono::milliseconds>(
                                          kNotifyPollInterval).count()) == WAIT_OBJECT_0)
        return;
      std::vector<ActiveRecord> due;
      {
        std::lock_guard lock(active_mutex_);
        auto now = std::chrono::steady_clock::now();
        for (auto it = active_.begin(); it != active_.end();) {
          if (now - it->second.touched > kActiveRecordLinger) it = active_.erase(it);
          else { due.push_back(it->second); ++it; }
        }
      }
      for (const auto& r : due) {
        try {
          files_locked(r.table, r.obj, true);   // refresh + diff -> notify
        } catch (const std::exception&) {
          // A blip must not kill the pump; the next tick retries.
        }
      }
    }
  }

  // Ephemeral plane pass-throughs, addressed by resolved record. The record's
  // tree cache is invalidated on every mutation so this client's listings
  // reflect it immediately; other clients converge within their tree TTL.
  bool eph_put(const std::string& table, const ObjectEntry& o, const std::string& inner,
               const std::string& bytes, std::string& err) {
    bool ok = ws_.ephemeral_put(table, o.numeric ? "id" : "guid",
                                o.numeric ? nlohmann::json(o.id) : nlohmann::json(o.key),
                                "/" + inner, bytes, &err);
    if (ok) invalidate_tree(table, o);
    return ok;
  }
  std::optional<std::string> eph_get(const std::string& table, const ObjectEntry& o,
                                     const std::string& inner) {
    return ws_.ephemeral_get(table, o.numeric ? "id" : "guid",
                             o.numeric ? nlohmann::json(o.id) : nlohmann::json(o.key),
                             "/" + inner);
  }
  bool eph_del(const std::string& table, const ObjectEntry& o, const std::string& inner) {
    bool ok = ws_.ephemeral_delete(table, o.numeric ? "id" : "guid",
                                   o.numeric ? nlohmann::json(o.id) : nlohmann::json(o.key),
                                   "/" + inner);
    if (ok) invalidate_tree(table, o);
    return ok;
  }
  void invalidate_tree(const std::string& table, const ObjectEntry& o) {
    std::lock_guard lock(mutex_);
    trees_.erase(lower_ascii(table) + "\x1f" + o.key);
  }

  // --- write-back plane -----------------------------------------------------

  // Upload a working copy and return its content id. The grant is minted per
  // upload (op=put, ~120 s) exactly like the download side.
  bool upload(const std::filesystem::path& src, std::string& hash_out, uint64_t& size_out,
              std::string& err) {
    nlohmann::json grant;
    try {
      grant = ws_.get_put_token();
    } catch (const std::exception& e) {
      err = e.what();
      return false;
    }
    if (!grant.contains("result")) {
      err = grant.value("error", "upload grant refused");
      return false;
    }
    auto res = grant["result"];
    if (res.value("mode", "") != "direct") {
      err = "direct byte plane unavailable";
      return false;
    }
    auto put = put_blob(res.value("urls", std::vector<std::string>{}),
                        res.value("fingerprint", ""), res.value("token", ""), src);
    if (!put.ok) {
      err = put.error;
      return false;
    }
    hash_out = put.hash;
    size_out = put.size;
    return true;
  }

  // The record's CURRENT server-side entry for a path, bypassing the tree TTL.
  // Used for last-moment conflict detection before re-pointing a row.
  std::optional<FileEntry> live_entry(const std::string& table, const ObjectEntry& o,
                                      const std::string& inner) {
    try {
      auto r = ws_.list_files(table, o.numeric ? "id" : "guid",
                              o.numeric ? nlohmann::json(o.id) : nlohmann::json(o.key));
      for (const auto& f : r.value("files", nlohmann::json::array())) {
        std::string path = f.value("filename", "");
        while (!path.empty() && (path.front() == '/' || path.front() == '\\')) path.erase(0, 1);
        if (lower_ascii(path) != lower_ascii(inner)) continue;
        FileEntry e;
        e.guid = f.value("guid", "");
        e.path = path;
        e.location = f.value("location", "");
        e.mimetype = f.value("mimetype", "");
        e.size = f.value("size", (uint64_t)0);
        e.can_write = f.value("can_write", false);
        e.ephemeral = f.value("ephemeral", false);
        return e;
      }
    } catch (const std::exception&) {}
    return std::nullopt;
  }

  bool add_file(const std::string& table, const ObjectEntry& o, const std::string& inner,
                const std::string& location, const std::string& mimetype, uint64_t size,
                std::string& err) {
    bool ok = ws_.add_attachment(table, o.numeric ? "id" : "guid",
                                 o.numeric ? nlohmann::json(o.id) : nlohmann::json(o.key),
                                 "/" + inner, location, mimetype, size, &err);
    if (ok) {
      if (!location.empty()) note_local_write(table, o, inner, location);
      invalidate_tree(table, o);
    }
    return ok;
  }
  bool repoint_file(const std::string& table, const ObjectEntry& o, const std::string& guid,
                    const std::string& location, uint64_t size, std::string& err,
                    const std::string& inner = {}) {
    bool ok = ws_.update_attachment_location(guid, location, size, &err);
    if (ok) {
      if (!inner.empty()) note_local_write(table, o, inner, location);
      invalidate_tree(table, o);
    }
    return ok;
  }
  bool rename_file(const std::string& table, const ObjectEntry& o, const std::string& guid,
                   const std::string& inner, std::string& err) {
    bool ok = ws_.rename_attachment(guid, "/" + inner, &err);
    if (ok) invalidate_tree(table, o);
    return ok;
  }
  bool delete_file(const std::string& table, const ObjectEntry& o, const std::string& guid,
                   std::string& err) {
    bool ok = ws_.delete_attachment(table, o.numeric ? "id" : "guid",
                                    o.numeric ? nlohmann::json(o.id) : nlohmann::json(o.key),
                                    guid, &err);
    if (ok) invalidate_tree(table, o);
    return ok;
  }

  const FileCache& cache() const { return cache_; }

  // Hydrate-on-open: cache hit, or a single fetch shared by every waiter.
  //
  // Two problems this solves beyond the plain fetch. First, browsing a folder
  // makes Explorer open the same file repeatedly -- without SINGLE-FLIGHT
  // that became a stampede of identical downloads. Second, a row whose blob
  // the daemon does not have can never succeed, so NEGATIVE CACHING stops
  // dozens of doomed round-trips (each paying a connect timeout per endpoint)
  // and lets the namespace mark the file as unavailable rather than pretend.
  struct HydrateResult {
    std::optional<std::filesystem::path> path;
    FetchFailure failure = FetchFailure::None;
    std::string error;
  };

  HydrateResult hydrate(const FileEntry& e) {
    HydrateResult out;
    if (!e.location.starts_with("sha256:")) {
      out.failure = FetchFailure::NotFound;
      out.error = "not daemon-backed";
      return out;
    }
    auto dest = cache_.path_for(e.location);
    if (cache_.present(e.location)) {
      out.path = dest;
      return out;
    }
    if (auto known = known_bad(e.location)) {
      out.failure = *known;
      out.error = "recently failed";
      return out;
    }

    // Single-flight: one fetch per hash, everyone else waits for it.
    std::unique_lock lock(fetch_mutex_);
    while (in_flight_.count(e.location)) {
      fetch_cv_.wait(lock);
      if (cache_.present(e.location)) {
        out.path = dest;
        return out;
      }
      if (auto known = known_bad(e.location)) {
        out.failure = *known;
        out.error = "recently failed";
        return out;
      }
    }
    in_flight_.insert(e.location);
    lock.unlock();

    auto finish = [&]() {
      std::lock_guard done(fetch_mutex_);
      in_flight_.erase(e.location);
      fetch_cv_.notify_all();
    };

    nlohmann::json grant;
    try {
      grant = ws_.get_file_token(e.guid);
    } catch (const std::exception& ex) {
      out.failure = FetchFailure::Transient;
      out.error = ex.what();
      note_bad(e.location, out.failure);
      finish();
      return out;
    }
    if (!grant.contains("result")) {
      out.error = grant.value("error", "grant refused");
      // The server refusing because the row is not daemon-backed is permanent;
      // anything else (no file service, bridge trouble) may recover.
      out.failure = out.error.find("not daemon-backed") != std::string::npos
                        ? FetchFailure::NotFound
                        : FetchFailure::Transient;
      note_bad(e.location, out.failure);
      finish();
      return out;
    }
    auto res = grant["result"];
    if (res.value("mode", "") != "direct") {
      out.failure = FetchFailure::Transient;
      out.error = "direct byte plane unavailable";
      note_bad(e.location, out.failure);
      finish();
      return out;
    }
    auto fetch = fetch_blob(res.value("urls", std::vector<std::string>{}),
                            res.value("fingerprint", ""), e.location,
                            res.value("token", ""), dest);
    if (!fetch.ok) {
      out.failure = fetch.failure;
      out.error = fetch.error;
      note_bad(e.location, out.failure);
      finish();
      return out;
    }
    forget_bad(e.location);
    finish();
    out.path = dest;
    return out;
  }

  // Content this mount has already failed to fetch. Missing blobs are
  // remembered far longer than blips: a dangling row will not heal, while a
  // daemon that was briefly unreachable should be retried soon.
  std::optional<FetchFailure> known_bad(const std::string& location) {
    std::lock_guard lock(bad_mutex_);
    auto it = bad_.find(location);
    if (it == bad_.end()) return std::nullopt;
    auto age = std::chrono::steady_clock::now() - it->second.when;
    auto ttl = it->second.failure == FetchFailure::NotFound ? kMissingBlobTtl
                                                            : kTransientFailureTtl;
    if (age > ttl) {
      bad_.erase(it);
      return std::nullopt;
    }
    return it->second.failure;
  }
  void note_bad(const std::string& location, FetchFailure f) {
    std::lock_guard lock(bad_mutex_);
    if (bad_.size() > 4096) bad_.clear();
    bad_[location] = BadBlob{f, std::chrono::steady_clock::now()};
  }
  void forget_bad(const std::string& location) {
    std::lock_guard lock(bad_mutex_);
    bad_.erase(location);
  }
  // True when this content is known to be permanently absent -- the namespace
  // marks such files OFFLINE so they do not look like ordinary readable data.
  bool content_missing(const std::string& location) {
    auto k = known_bad(location);
    return k && *k == FetchFailure::NotFound;
  }

private:
  struct FormerName {
    std::string key;
    std::chrono::steady_clock::time_point retired;
  };
  struct ObjectsCache {
    std::vector<ObjectEntry> entries;
    std::map<std::string, size_t> by_name;
    // Directory names records USED to answer to, kept briefly after a rename.
    // Applications capture a path at open and re-open by that path later --
    // the Office/CAD save dance is create-temp / delete-original / rename --
    // so without this a rename mid-edit turns the next save into
    // STATUS_OBJECT_NAME_NOT_FOUND. Resolution is by KEY, so the stale path
    // reaches the record the user meant even though its name moved.
    std::map<std::string, FormerName> former;
    std::chrono::steady_clock::time_point fetched{};
  };
  struct TreeCache {
    std::shared_ptr<const FileTree> tree;
    std::chrono::steady_clock::time_point fetched{};
  };

  WsClient& ws_;
  FileCache& cache_;
  uint64_t mount_time_;
  std::mutex tables_mutex_;
  std::vector<std::string> tables_;
  std::map<std::string, bool> writable_;   // lower(table) -> table write mask
  std::chrono::steady_clock::time_point fetched_tables_{};

  std::mutex mutex_;
  std::map<std::string, ObjectsCache> objects_;
  std::map<std::string, TreeCache> trees_;

  // Single-flight downloads.
  std::mutex fetch_mutex_;
  std::condition_variable fetch_cv_;
  std::set<std::string> in_flight_;

  // Negative cache for content that could not be fetched.
  struct BadBlob {
    FetchFailure failure;
    std::chrono::steady_clock::time_point when;
  };
  std::mutex bad_mutex_;
  std::map<std::string, BadBlob> bad_;

  // Records the notify pump is watching (anything touched recently).
  struct ActiveRecord {
    std::string table;
    ObjectEntry obj;
    std::chrono::steady_clock::time_point touched;
  };
  std::mutex active_mutex_;
  std::map<std::string, ActiveRecord> active_;

  // (table, key, inner) -> content this machine just wrote.
  std::mutex local_writes_mutex_;
  std::map<std::string, std::string> local_writes_;
};

// ---------------------------------------------------------------------------
// path resolution

struct Resolved {
  enum Kind { Root, TableDir, ObjectDir, InnerDir, File } kind = Root;
  std::string table;
  ObjectEntry object;
  std::string inner;                      // path within the record ("" = record root)
  std::shared_ptr<const FileTree> tree;   // ObjectDir/InnerDir/File
  std::optional<FileEntry> file;          // File (copied out of the tree)
};

std::vector<std::string> split_backslash(const std::string& p) {
  std::vector<std::string> parts;
  std::string cur;
  for (char c : p) {
    if (c == '\\' || c == '/') {
      if (!cur.empty()) parts.push_back(std::move(cur));
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) parts.push_back(std::move(cur));
  return parts;
}

std::optional<Resolved> resolve(NamespaceService& ns, const wchar_t* wpath) {
  Resolved r;
  auto parts = split_backslash(narrow(wpath));
  if (parts.empty()) {
    r.kind = Resolved::Root;
    return r;
  }
  // component 0: table
  r.table.clear();
  for (const auto& t : ns.tables())
    if (lower_ascii(t) == lower_ascii(parts[0])) r.table = t;
  if (r.table.empty()) return std::nullopt;
  if (parts.size() == 1) {
    r.kind = Resolved::TableDir;
    return r;
  }
  // component 1: record
  auto obj = ns.find_object(r.table, parts[1]);
  if (!obj) return std::nullopt;
  r.object = *obj;
  r.tree = ns.files(r.table, r.object);
  if (parts.size() == 2) {
    r.kind = Resolved::ObjectDir;
    return r;
  }
  // components 2..: inside the record
  std::string inner;
  for (size_t i = 2; i < parts.size(); ++i) {
    if (i > 2) inner.push_back('/');
    inner += parts[i];
  }
  auto child = r.tree->find(inner);
  if (!child) return std::nullopt;
  r.inner = inner;
  if (child->is_dir) {
    r.kind = Resolved::InnerDir;
  } else {
    r.kind = Resolved::File;
    r.file = *child->file;
  }
  return r;
}

// ---------------------------------------------------------------------------
// the filesystem

struct FsContext {  // per-open-handle
  Resolved res;
  HANDLE local = INVALID_HANDLE_VALUE;  // hydrated cache file (durable, read)
  PVOID dir_buffer = nullptr;
  // Ephemeral file state: bytes live in this buffer between open and close;
  // cleanup writes them back to the server plane.
  bool is_eph = false;
  std::string eph_buf;
  bool dirty = false;
  bool deleted = false;

  // Durable write-back (P2). Opening a durable file for write copies the
  // hydrated blob up into a private WORKING COPY; writes land there, and
  // cleanup uploads it and re-points (or creates) the attachment row. Blobs
  // are immutable and content-addressed, so the original is never mutated.
  bool is_write = false;                 // working copy is live
  bool is_new = false;                   // created here; needs add_attachment
  HANDLE work = INVALID_HANDLE_VALUE;
  std::filesystem::path work_path;
  std::string base_hash;                 // content id this copy started from
  std::string guid;                      // attachment row (empty when is_new)
  std::string mimetype;
};

// Per-open working copy under %LOCALAPPDATA%\RecordFS\work.
std::filesystem::path new_work_path() {
  static std::atomic<uint64_t> seq{0};
  std::filesystem::path dir;
  char* base = nullptr;
  size_t len = 0;
  if (_dupenv_s(&base, &len, "LOCALAPPDATA") == 0 && base) {
    dir = std::filesystem::path(base) / "RecordFS" / "work";
    free(base);
  } else {
    dir = std::filesystem::temp_directory_path() / "RecordFS" / "work";
  }
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  return dir / (std::to_string(::GetCurrentProcessId()) + "-" +
                std::to_string(seq.fetch_add(1)) + ".work");
}

// The server stores mimetype in a 64-char column, and the official
// OpenXML types are LONGER than that (xlsx is 65, docx 71) -- emitting one
// makes the attachment row insert fail and the save vanish with only a log
// line. Office formats therefore map to the generic type (which is what the
// desktop client stores anyway), and anything over the limit falls back
// defensively so a future addition cannot silently break saves.
constexpr size_t kMaxMimetype = 64;

std::string guess_mimetype(const std::string& leaf) {
  auto dot = leaf.rfind('.');
  std::string ext = dot == std::string::npos ? "" : lower_ascii(leaf.substr(dot + 1));
  static const std::map<std::string, std::string> kTypes = {
      {"pdf","application/pdf"}, {"jpg","image/jpeg"}, {"jpeg","image/jpeg"},
      {"png","image/png"}, {"gif","image/gif"}, {"bmp","image/bmp"},
      {"tif","image/tiff"}, {"tiff","image/tiff"},
      {"txt","text/plain"}, {"log","text/plain"}, {"csv","text/csv"},
      {"xml","text/xml"}, {"html","text/html"}, {"json","application/json"},
      {"dwg","image/vnd.dwg"}, {"dxf","image/vnd.dxf"},
      {"doc","application/msword"}, {"xls","application/vnd.ms-excel"},
      {"zip","application/zip"}};
  auto it = kTypes.find(ext);
  std::string t = it == kTypes.end() ? "application/octet-stream" : it->second;
  if (t.size() > kMaxMimetype) t = "application/octet-stream";
  return t;
}

// Open (creating if needed) the working copy for a handle.
bool open_work(FsContext* ctx, const std::filesystem::path& seed, std::string& err) {
  ctx->work_path = new_work_path();
  if (!seed.empty()) {
    std::error_code ec;
    std::filesystem::copy_file(seed, ctx->work_path,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) { err = "copy-up: " + ec.message(); return false; }
  }
  ctx->work = ::CreateFileW(ctx->work_path.c_str(), GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ, nullptr,
                            seed.empty() ? CREATE_ALWAYS : OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (ctx->work == INVALID_HANDLE_VALUE) { err = "working copy open failed"; return false; }
  ctx->is_write = true;
  return true;
}

void discard_work(FsContext* ctx) {
  if (ctx->work != INVALID_HANDLE_VALUE) {
    ::CloseHandle(ctx->work);
    ctx->work = INVALID_HANDLE_VALUE;
  }
  if (!ctx->work_path.empty()) {
    std::error_code ec;
    std::filesystem::remove(ctx->work_path, ec);
    ctx->work_path.clear();
  }
}

// The D2 pattern gate: which names are ephemeral (server-memory lock/temp
// files, writable) vs durable attachments (read-only until the P2 write-back
// engine). Matches Office owner files (~$*) and classic temp/lock suffixes.
bool is_ephemeral_name(const std::string& leaf) {
  std::string l = lower_ascii(leaf);
  if (l.rfind("~$", 0) == 0) return true;
  for (const char* suf : {".tmp", ".dwl", ".dwl2", ".laccdb", ".ldb"})
    if (l.size() > strlen(suf) && l.ends_with(suf)) return true;
  return false;
}

struct Volume {
  NamespaceService* ns = nullptr;
  FSP_FILE_SYSTEM* fs = nullptr;
  std::wstring label;
  PSECURITY_DESCRIPTOR sd = nullptr;
  ULONG sd_size = 0;
};
Volume g_vol;

// Report a change under <table>\<record>\<inner> to anyone watching that
// directory. WinFsp requires the Begin/Notify/End bracket, and Begin can
// refuse while a rename is in flight -- that is not an error, just skip this
// round; the pump will notice the same difference next tick.
void notify_change(const std::string& table, const std::string& dir_name,
                   const std::string& inner, UINT32 action) {
  if (!g_vol.fs) return;
  std::string rel = table + "\\" + dir_name + "\\" + inner;
  for (auto& c : rel)
    if (c == '/') c = '\\';
  // WinFsp requires NORMALIZED names here. This volume is case-insensitive and
  // does not implement name normalization, for which the documented normal
  // form is UPPER CASE -- and it is what the driver itself emits for local
  // changes. Passing the natural case silently delivers nothing.
  std::wstring wpath = L"\\" + widen(rel);
  for (auto& wc : wpath) wc = (wchar_t)::towupper(wc);

  NTSTATUS st = FspFileSystemNotifyBegin(g_vol.fs, 0);
  if (!NT_SUCCESS(st)) return;   // busy (e.g. STATUS_CANT_WAIT); try next tick

  // Flexible array member: stage in a byte buffer and cast (it cannot live in
  // a union under MSVC).
  UINT8 buf[sizeof(FSP_FSCTL_NOTIFY_INFO) + 512 * sizeof(WCHAR)] = {};
  auto* ni = (FSP_FSCTL_NOTIFY_INFO*)buf;
  size_t namelen = std::min<size_t>(wpath.size(), 511);
  ni->Size = (UINT16)(sizeof(FSP_FSCTL_NOTIFY_INFO) + namelen * sizeof(WCHAR));
  ni->Filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE |
               FILE_NOTIFY_CHANGE_LAST_WRITE;
  ni->Action = action;
  std::memcpy(ni->FileNameBuf, wpath.c_str(), namelen * sizeof(WCHAR));
  FspFileSystemNotify(g_vol.fs, ni, ni->Size);
  FspFileSystemNotifyEnd(g_vol.fs);

  const char* what = action == FILE_ACTION_ADDED ? "added"
                   : action == FILE_ACTION_REMOVED ? "removed" : "modified";
  info(std::string("peer change (") + what + "): " + rel);
}

void fill_file_info(const Resolved& r, FSP_FSCTL_FILE_INFO* info) {
  std::memset(info, 0, sizeof(*info));
  uint64_t t0 = g_vol.ns->mount_time();
  info->CreationTime = info->LastAccessTime = info->LastWriteTime = info->ChangeTime = t0;
  if (r.kind == Resolved::File && r.file) {
    // Writability now follows the server's per-attachment mask: the write-back
    // engine handles durable saves, so only a mask denial marks a file
    // read-only. Ephemeral (lock/temp) files are always writable.
    info->FileAttributes = (r.file->ephemeral || r.file->can_write)
                               ? FILE_ATTRIBUTE_ARCHIVE
                               : FILE_ATTRIBUTE_READONLY;
    // Content we know the file service does not have: show it as offline
    // rather than as ordinary readable data (it opens with a real error).
    if (!r.file->ephemeral && g_vol.ns && g_vol.ns->content_missing(r.file->location))
      info->FileAttributes |= FILE_ATTRIBUTE_OFFLINE;
    info->FileSize = r.file->size;
    info->AllocationSize = (r.file->size + 4095) / 4096 * 4096;
    uint64_t mt = parse_timestamp(r.file->modified_on, t0);
    info->LastWriteTime = info->ChangeTime = info->CreationTime = mt;
  } else {
    info->FileAttributes = FILE_ATTRIBUTE_DIRECTORY;
    if (r.kind == Resolved::Root || r.kind == Resolved::TableDir)
      info->FileAttributes |= FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;
  }
}

NTSTATUS SvcGetVolumeInfo(FSP_FILE_SYSTEM*, FSP_FSCTL_VOLUME_INFO* v) {
  v->TotalSize = 1ull << 40;
  v->FreeSize = 1ull << 39;
  // Configurable label (installer policy / --volume); the field is a fixed
  // 32-WCHAR buffer, so a long name is truncated rather than overrunning.
  std::wstring label = g_vol.label.empty() ? L"Records" : g_vol.label;
  const size_t max_chars = sizeof(v->VolumeLabel) / sizeof(WCHAR) - 1;
  if (label.size() > max_chars) label.resize(max_chars);
  std::memcpy(v->VolumeLabel, label.c_str(), (label.size() + 1) * sizeof(WCHAR));
  v->VolumeLabelLength = (UINT16)(label.size() * sizeof(WCHAR));
  return STATUS_SUCCESS;
}

NTSTATUS SvcGetSecurityByName(FSP_FILE_SYSTEM*, PWSTR file_name, PUINT32 attributes,
                              PSECURITY_DESCRIPTOR sd, SIZE_T* sd_size) {
  auto r = resolve(*g_vol.ns, file_name);
  if (!r) return STATUS_OBJECT_NAME_NOT_FOUND;
  if (attributes) {
    FSP_FSCTL_FILE_INFO info;
    fill_file_info(*r, &info);
    *attributes = info.FileAttributes;
  }
  if (sd_size) {
    if (g_vol.sd_size > *sd_size) {
      *sd_size = g_vol.sd_size;
      return STATUS_BUFFER_OVERFLOW;
    }
    if (sd) std::memcpy(sd, g_vol.sd, g_vol.sd_size);
    *sd_size = g_vol.sd_size;
  }
  return STATUS_SUCCESS;
}

// Fill info for an open durable working copy (the copy is the truth).
void fill_work_info(const FsContext* ctx, FSP_FSCTL_FILE_INFO* info) {
  std::memset(info, 0, sizeof(*info));
  info->FileAttributes = FILE_ATTRIBUTE_ARCHIVE;
  LARGE_INTEGER sz{};
  if (ctx->work != INVALID_HANDLE_VALUE) ::GetFileSizeEx(ctx->work, &sz);
  info->FileSize = (UINT64)sz.QuadPart;
  info->AllocationSize = (info->FileSize + 4095) / 4096 * 4096;
  uint64_t t = now_filetime();
  info->CreationTime = info->LastAccessTime = info->LastWriteTime = info->ChangeTime = t;
}

// Fill info for an open ephemeral handle (its live buffer is the truth).
void fill_eph_info(const FsContext* ctx, FSP_FSCTL_FILE_INFO* info) {
  std::memset(info, 0, sizeof(*info));
  info->FileAttributes = FILE_ATTRIBUTE_ARCHIVE;
  info->FileSize = ctx->eph_buf.size();
  info->AllocationSize = (info->FileSize + 4095) / 4096 * 4096;
  uint64_t t = now_filetime();
  info->CreationTime = info->LastAccessTime = info->LastWriteTime = info->ChangeTime = t;
}

constexpr size_t kEphMaxBytes = 1 << 20;  // matches the server-side cap

// New files inside a record. Lock/temp names (the D2 pattern) go to the
// server-memory ephemeral plane; everything else becomes a real attachment,
// written through a working copy and registered at cleanup.
//
// The parent is resolved as RECORD + remaining path rather than as an
// existing directory: directories here are implied by attachment paths, so
// "<record>/sub/file.txt" must work whether or not "sub" exists yet (apps
// call CreateDirectory then immediately create inside it).
// The Create/Open/Overwrite trio must all exist or the WinFsp Create
// dispatcher refuses every open with IoStatus=c0000010.
NTSTATUS SvcCreate(FSP_FILE_SYSTEM*, PWSTR file_name, UINT32 create_options, UINT32,
                   UINT32, PSECURITY_DESCRIPTOR, UINT64, PVOID* file_context,
                   FSP_FSCTL_FILE_INFO* info) {
  std::string full = narrow(file_name);
  auto parts = split_backslash(full);
  if (parts.size() < 3) return STATUS_MEDIA_WRITE_PROTECTED;  // root / table level

  std::string table;
  for (const auto& t : g_vol.ns->tables())
    if (lower_ascii(t) == lower_ascii(parts[0])) table = t;
  if (table.empty()) return STATUS_OBJECT_PATH_NOT_FOUND;
  auto obj = g_vol.ns->find_object(table, parts[1]);
  if (!obj) return STATUS_OBJECT_PATH_NOT_FOUND;

  std::string inner;
  for (size_t i = 2; i < parts.size(); ++i) {
    if (i > 2) inner.push_back('/');
    inner += parts[i];
  }
  std::string leaf = parts.back();

  // A table the user may read but not write is read-only on the drive too.
  // Refuse here rather than accepting the write and having the server reject
  // it at cleanup, which would look like a save that silently vanished.
  // Ephemeral lock/temp files stay allowed: they are coordination state, not
  // record data, and refusing them breaks opening documents read-only.
  const bool writable = g_vol.ns->table_writable(table);
  if (!writable && !is_ephemeral_name(leaf)) return STATUS_ACCESS_DENIED;

  auto ctx = std::make_unique<FsContext>();
  ctx->res.kind = Resolved::File;
  ctx->res.table = table;
  ctx->res.object = *obj;
  ctx->res.inner = inner;
  ctx->res.tree = g_vol.ns->files(table, *obj);

  if (create_options & FILE_DIRECTORY_FILE) {
    // Persist an explicit directory row (the shape the desktop writes, and
    // exempt from the byte-plane location check server-side) so an empty
    // folder survives until something lands in it. Only when it does not
    // already exist -- either as its own row or implied by some attachment's
    // path -- otherwise every CreateDirectory on an existing folder would
    // add ANOTHER row, and one rmdir would leave the duplicates behind.
    bool implied = ctx->res.tree && ctx->res.tree->find(inner).has_value();
    if (!implied && !g_vol.ns->live_entry(table, *obj, inner)) {
      std::string err;
      if (!g_vol.ns->add_file(table, *obj, inner, "", "inode/directory", 0, err))
        warn("mkdir " + inner + ": " + err);
    }
    ctx->res.kind = Resolved::InnerDir;
    fill_file_info(ctx->res, info);
    *file_context = ctx.release();
    return STATUS_SUCCESS;
  }

  if (is_ephemeral_name(leaf)) {
    ctx->is_eph = true;
    ctx->dirty = true;  // an empty create still registers at cleanup
    std::string err;
    if (!g_vol.ns->eph_put(table, *obj, inner, "", err)) {
      warn("ephemeral create " + inner + ": " + err);
      return STATUS_IO_DEVICE_ERROR;
    }
    fill_eph_info(ctx.get(), info);
    *file_context = ctx.release();
    return STATUS_SUCCESS;
  }

  // Durable create: an empty working copy now, add_attachment at cleanup --
  // a row cannot exist before its bytes have a content id.
  std::string err;
  if (!open_work(ctx.get(), {}, err)) {
    warn("create " + inner + ": " + err);
    return STATUS_IO_DEVICE_ERROR;
  }
  ctx->is_new = true;
  ctx->dirty = true;
  ctx->mimetype = guess_mimetype(leaf);
  fill_work_info(ctx.get(), info);
  *file_context = ctx.release();
  return STATUS_SUCCESS;
}

NTSTATUS SvcOverwrite(FSP_FILE_SYSTEM*, PVOID file_context, UINT32, BOOLEAN, UINT64,
                      FSP_FSCTL_FILE_INFO* info) {
  auto* ctx = (FsContext*)file_context;
  if (!ctx) return STATUS_INVALID_DEVICE_REQUEST;
  if (ctx->is_eph) {
    ctx->eph_buf.clear();
    ctx->dirty = true;
    fill_eph_info(ctx, info);
    return STATUS_SUCCESS;
  }
  if (ctx->is_write) {
    ::SetFilePointer(ctx->work, 0, nullptr, FILE_BEGIN);
    ::SetEndOfFile(ctx->work);
    ctx->dirty = true;
    fill_work_info(ctx, info);
    return STATUS_SUCCESS;
  }
  return STATUS_ACCESS_DENIED;
}

NTSTATUS SvcWrite(FSP_FILE_SYSTEM*, PVOID file_context, PVOID buffer, UINT64 offset,
                  ULONG length, BOOLEAN write_to_eof, BOOLEAN constrained,
                  PULONG bytes_transferred, FSP_FSCTL_FILE_INFO* info) {
  auto* ctx = (FsContext*)file_context;
  if (!ctx) return STATUS_INVALID_DEVICE_REQUEST;

  if (ctx->is_write) {  // durable working copy
    LARGE_INTEGER cur{};
    ::GetFileSizeEx(ctx->work, &cur);
    uint64_t size = (uint64_t)cur.QuadPart;
    uint64_t off = write_to_eof ? size : offset;
    ULONG len = length;
    if (constrained) {
      if (off >= size) {
        *bytes_transferred = 0;
        fill_work_info(ctx, info);
        return STATUS_SUCCESS;
      }
      len = (ULONG)std::min<uint64_t>(len, size - off);
    }
    OVERLAPPED ov{};
    ov.Offset = (DWORD)(off & 0xFFFFFFFF);
    ov.OffsetHigh = (DWORD)(off >> 32);
    DWORD n = 0;
    if (!::WriteFile(ctx->work, buffer, len, &n, &ov)) return STATUS_IO_DEVICE_ERROR;
    ctx->dirty = true;
    *bytes_transferred = n;
    fill_work_info(ctx, info);
    return STATUS_SUCCESS;
  }

  if (!ctx->is_eph) return STATUS_ACCESS_DENIED;
  size_t off = write_to_eof ? ctx->eph_buf.size() : (size_t)offset;
  size_t len = length;
  if (constrained) {
    if (off >= ctx->eph_buf.size()) {
      *bytes_transferred = 0;
      fill_eph_info(ctx, info);
      return STATUS_SUCCESS;
    }
    len = std::min(len, ctx->eph_buf.size() - off);
  }
  if (off + len > kEphMaxBytes) return STATUS_DISK_FULL;
  if (off + len > ctx->eph_buf.size()) ctx->eph_buf.resize(off + len, '\0');
  std::memcpy(ctx->eph_buf.data() + off, buffer, len);
  ctx->dirty = true;
  *bytes_transferred = (ULONG)len;
  fill_eph_info(ctx, info);
  return STATUS_SUCCESS;
}

NTSTATUS SvcSetFileSize(FSP_FILE_SYSTEM*, PVOID file_context, UINT64 new_size,
                        BOOLEAN allocation_only, FSP_FSCTL_FILE_INFO* info) {
  auto* ctx = (FsContext*)file_context;
  if (!ctx) return STATUS_INVALID_DEVICE_REQUEST;
  if (ctx->is_write) {
    if (!allocation_only) {
      LARGE_INTEGER li;
      li.QuadPart = (LONGLONG)new_size;
      ::SetFilePointerEx(ctx->work, li, nullptr, FILE_BEGIN);
      ::SetEndOfFile(ctx->work);
      ctx->dirty = true;
    }
    fill_work_info(ctx, info);
    return STATUS_SUCCESS;
  }
  if (!ctx->is_eph) return STATUS_ACCESS_DENIED;
  if (!allocation_only) {
    if (new_size > kEphMaxBytes) return STATUS_DISK_FULL;
    ctx->eph_buf.resize((size_t)new_size, '\0');
    ctx->dirty = true;
  }
  fill_eph_info(ctx, info);
  return STATUS_SUCCESS;
}

NTSTATUS SvcSetBasicInfo(FSP_FILE_SYSTEM*, PVOID file_context, UINT32, UINT64, UINT64,
                         UINT64, UINT64, FSP_FSCTL_FILE_INFO* info) {
  auto* ctx = (FsContext*)file_context;
  if (!ctx) return STATUS_INVALID_DEVICE_REQUEST;
  // Apps set attributes/times constantly; accept and ignore. Server-side
  // timestamps are authoritative and not worth a round-trip per touch.
  if (ctx->is_eph) fill_eph_info(ctx, info);
  else if (ctx->is_write) fill_work_info(ctx, info);
  else fill_file_info(ctx->res, info);
  return STATUS_SUCCESS;
}

NTSTATUS SvcCanDelete(FSP_FILE_SYSTEM*, PVOID file_context, PWSTR) {
  auto* ctx = (FsContext*)file_context;
  if (!ctx) return STATUS_INVALID_DEVICE_REQUEST;
  if (ctx->is_eph) return STATUS_SUCCESS;
  if (ctx->is_new) return STATUS_SUCCESS;        // never registered
  if (!g_vol.ns->table_writable(ctx->res.table)) return STATUS_ACCESS_DENIED;
  if (ctx->res.kind == Resolved::InnerDir) {
    // A directory is a real inode/directory row; removable once empty.
    if (ctx->res.tree && !ctx->res.tree->list(ctx->res.inner).empty())
      return STATUS_DIRECTORY_NOT_EMPTY;
    return STATUS_SUCCESS;
  }
  if (ctx->res.kind == Resolved::File && ctx->res.file && ctx->res.file->can_write)
    return STATUS_SUCCESS;                       // soft-deleted at cleanup
  return STATUS_ACCESS_DENIED;
}

// A conflict-copy name: "plan (conflict 2026-08-31 141233).dwg".
std::string conflict_name(const std::string& inner) {
  auto slash = inner.find_last_of('/');
  std::string dir = slash == std::string::npos ? "" : inner.substr(0, slash + 1);
  std::string leaf = slash == std::string::npos ? inner : inner.substr(slash + 1);
  auto dot = leaf.rfind('.');
  std::string stem = dot == std::string::npos ? leaf : leaf.substr(0, dot);
  std::string ext = dot == std::string::npos ? "" : leaf.substr(dot);
  SYSTEMTIME st;
  ::GetLocalTime(&st);
  char stamp[32];
  sprintf_s(stamp, "%04u-%02u-%02u %02u%02u%02u", st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond);
  return dir + stem + " (conflict " + stamp + ")" + ext;
}

// Upload the working copy and land it as metadata. Runs at CLEANUP (the
// app's close), never at Close: the kernel defers the final Close IRP, so a
// flush parked there loses the race against an immediate reopen.
void write_back(FsContext* ctx) {
  ::FlushFileBuffers(ctx->work);
  ::CloseHandle(ctx->work);
  ctx->work = INVALID_HANDLE_VALUE;

  // Unchanged bytes are a no-op save: content addressing makes that free to
  // detect, and it keeps app "save" clicks that changed nothing off the wire.
  std::string local_hash = "sha256:" + sha256_hex_of_file(ctx->work_path);
  if (!ctx->is_new && local_hash == ctx->base_hash) {
    info("no-op save (unchanged): " + ctx->res.inner);
    return;
  }

  std::string err, hash;
  uint64_t size = 0;
  if (!g_vol.ns->upload(ctx->work_path, hash, size, err)) {
    error("upload " + ctx->res.inner + ": " + err + " -- working copy kept at " +
          ctx->work_path.string());
    ctx->work_path.clear();  // keep it: the user's bytes are in there
    return;
  }

  if (ctx->is_new) {
    if (!g_vol.ns->add_file(ctx->res.table, ctx->res.object, ctx->res.inner, hash,
                            ctx->mimetype, size, err))
      error("add " + ctx->res.inner + ": " + err);
    else
      info("created " + ctx->res.inner + " (" + std::to_string(size) + " bytes)");
    return;
  }

  // Conflict check against the CURRENT row, not the cached tree: if someone
  // else re-pointed it since we copied up, land ours beside theirs instead of
  // silently overwriting (D3 conflict copies).
  auto live = g_vol.ns->live_entry(ctx->res.table, ctx->res.object, ctx->res.inner);
  if (live && !live->location.empty() && live->location != ctx->base_hash) {
    std::string cname = conflict_name(ctx->res.inner);
    warn("conflict on " + ctx->res.inner + " (changed since open) -- saving as " + cname);
    if (!g_vol.ns->add_file(ctx->res.table, ctx->res.object, cname, hash,
                            ctx->mimetype.empty() ? live->mimetype : ctx->mimetype, size, err))
      error("conflict copy " + cname + ": " + err);
    return;
  }

  std::string guid = !ctx->guid.empty() ? ctx->guid : (live ? live->guid : std::string());
  if (guid.empty()) {
    error("save " + ctx->res.inner + ": attachment vanished");
    return;
  }
  if (!g_vol.ns->repoint_file(ctx->res.table, ctx->res.object, guid, hash, size, err,
                              ctx->res.inner))
    error("save " + ctx->res.inner + ": " + err);
  else
    info("saved " + ctx->res.inner + " (" + std::to_string(size) + " bytes)");
}

VOID SvcCleanup(FSP_FILE_SYSTEM*, PVOID file_context, PWSTR, ULONG flags) {
  auto* ctx = (FsContext*)file_context;
  if (!ctx) return;

  if (flags & FspCleanupDelete) {
    if (ctx->deleted) return;
    ctx->deleted = true;
    std::string err;
    if (ctx->is_eph) {
      if (!g_vol.ns->eph_del(ctx->res.table, ctx->res.object, ctx->res.inner))
        warn("ephemeral delete " + ctx->res.inner + " failed");
    } else if (ctx->is_write && ctx->is_new) {
      discard_work(ctx);                      // never registered; nothing to delete
    } else {
      std::string guid = ctx->guid;
      if (guid.empty() && ctx->res.file) guid = ctx->res.file->guid;
      if (guid.empty()) {   // directories, and anything the cached tree missed
        auto live = g_vol.ns->live_entry(ctx->res.table, ctx->res.object, ctx->res.inner);
        if (live) guid = live->guid;
      }
      if (guid.empty() ||
          !g_vol.ns->delete_file(ctx->res.table, ctx->res.object, guid, err))
        warn("delete " + ctx->res.inner + ": " + (err.empty() ? "no attachment" : err));
      else
        info("deleted " + ctx->res.inner + " (recoverable within retention)");
      discard_work(ctx);
    }
    return;
  }

  // Cleanup is the app's close and runs synchronously with it -- flush HERE.
  if (!ctx->dirty) return;
  if (ctx->is_eph) {
    std::string err;
    if (g_vol.ns->eph_put(ctx->res.table, ctx->res.object, ctx->res.inner, ctx->eph_buf, err))
      ctx->dirty = false;
    else
      warn("ephemeral write-back " + ctx->res.inner + ": " + err);
    return;
  }
  if (ctx->is_write) {
    write_back(ctx);
    ctx->dirty = false;
    ctx->is_write = false;   // the working copy is consumed
    discard_work(ctx);
  }
}

NTSTATUS SvcOpen(FSP_FILE_SYSTEM*, PWSTR file_name, UINT32, UINT32 granted_access,
                 PVOID* file_context, FSP_FSCTL_FILE_INFO* info) {
  auto r = resolve(*g_vol.ns, file_name);
  if (!r) return STATUS_OBJECT_NAME_NOT_FOUND;

  auto ctx = std::make_unique<FsContext>();
  ctx->res = *r;

  if (r->kind == Resolved::File && r->file->ephemeral) {
    ctx->is_eph = true;
    auto bytes = g_vol.ns->eph_get(r->table, r->object, r->file->path);
    if (!bytes) return STATUS_OBJECT_NAME_NOT_FOUND;  // vanished (owner died)
    ctx->eph_buf = std::move(*bytes);
    ctx->res.inner = r->file->path;
    fill_eph_info(ctx.get(), info);
    *file_context = ctx.release();
    return STATUS_SUCCESS;
  }

  if (r->kind == Resolved::File) {
    const bool wants_write =
        (granted_access & (FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE)) != 0;
    if (wants_write && !r->file->can_write) return STATUS_ACCESS_DENIED;

    auto h = g_vol.ns->hydrate(*r->file);
    if (!h.path) {
      // Report what is actually wrong. STATUS_IO_DEVICE_ERROR reads to users
      // as failing hardware; neither of these is that.
      if (h.failure == FetchFailure::NotFound) {
        warn("content missing for " + r->file->path + " (" + h.error +
             ") -- the attachment row points at bytes the file service does not have");
        return STATUS_FILE_CORRUPT_ERROR;   // "corrupted and unreadable"
      }
      warn("hydrate " + r->file->path + ": " + h.error);
      return STATUS_UNEXPECTED_NETWORK_ERROR;
    }
    auto local = h.path;
    ctx->res.inner = r->file->path;
    ctx->guid = r->file->guid;
    ctx->base_hash = r->file->location;
    ctx->mimetype = r->file->mimetype;

    if (wants_write) {
      // Copy up: writes never touch the shared, immutable cache blob.
      std::string err;
      if (!open_work(ctx.get(), *local, err)) {
        warn("copy-up " + r->file->path + ": " + err);
        return STATUS_IO_DEVICE_ERROR;
      }
      fill_work_info(ctx.get(), info);
      *file_context = ctx.release();
      return STATUS_SUCCESS;
    }
    ctx->local = ::CreateFileW(local->c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (ctx->local == INVALID_HANDLE_VALUE) return STATUS_IO_DEVICE_ERROR;
  }
  fill_file_info(*r, info);
  *file_context = ctx.release();
  return STATUS_SUCCESS;
}

VOID SvcClose(FSP_FILE_SYSTEM*, PVOID file_context) {
  auto* ctx = (FsContext*)file_context;
  if (!ctx) return;
  if (ctx->local != INVALID_HANDLE_VALUE) ::CloseHandle(ctx->local);
  discard_work(ctx);
  if (ctx->dir_buffer) FspFileSystemDeleteDirectoryBuffer(&ctx->dir_buffer);
  delete ctx;
}

NTSTATUS SvcRead(FSP_FILE_SYSTEM*, PVOID file_context, PVOID buffer, UINT64 offset, ULONG length,
                 PULONG bytes_transferred) {
  auto* ctx = (FsContext*)file_context;
  if (!ctx) return STATUS_INVALID_DEVICE_REQUEST;

  if (ctx->is_eph) {
    if (offset >= ctx->eph_buf.size()) { *bytes_transferred = 0; return STATUS_END_OF_FILE; }
    size_t n = std::min<size_t>(length, ctx->eph_buf.size() - (size_t)offset);
    std::memcpy(buffer, ctx->eph_buf.data() + offset, n);
    *bytes_transferred = (ULONG)n;
    return STATUS_SUCCESS;
  }

  // A writer must read back its own unflushed writes.
  HANDLE h = ctx->is_write ? ctx->work : ctx->local;
  if (h == INVALID_HANDLE_VALUE) return STATUS_INVALID_DEVICE_REQUEST;
  OVERLAPPED ov{};
  ov.Offset = (DWORD)(offset & 0xFFFFFFFF);
  ov.OffsetHigh = (DWORD)(offset >> 32);
  DWORD n = 0;
  if (!::ReadFile(h, buffer, length, &n, &ov)) {
    if (::GetLastError() == ERROR_HANDLE_EOF) { *bytes_transferred = 0; return STATUS_END_OF_FILE; }
    return STATUS_IO_DEVICE_ERROR;
  }
  if (n == 0) { *bytes_transferred = 0; return STATUS_END_OF_FILE; }
  *bytes_transferred = n;
  return STATUS_SUCCESS;
}

NTSTATUS SvcGetFileInfo(FSP_FILE_SYSTEM*, PVOID file_context, FSP_FSCTL_FILE_INFO* info) {
  auto* ctx = (FsContext*)file_context;
  if (!ctx) return STATUS_INVALID_DEVICE_REQUEST;
  if (ctx->is_eph) fill_eph_info(ctx, info);
  else if (ctx->is_write) fill_work_info(ctx, info);
  else fill_file_info(ctx->res, info);
  return STATUS_SUCCESS;
}

NTSTATUS SvcGetSecurity(FSP_FILE_SYSTEM*, PVOID, PSECURITY_DESCRIPTOR sd, SIZE_T* sd_size) {
  if (sd_size) {
    if (g_vol.sd_size > *sd_size) {
      *sd_size = g_vol.sd_size;
      return STATUS_BUFFER_OVERFLOW;
    }
    if (sd) std::memcpy(sd, g_vol.sd, g_vol.sd_size);
    *sd_size = g_vol.sd_size;
  }
  return STATUS_SUCCESS;
}

// One FSP_FSCTL_DIR_INFO into the directory buffer.
bool add_dir_entry(PVOID* dir_buffer, const std::wstring& name, const FSP_FSCTL_FILE_INFO& info,
                   NTSTATUS* result) {
  UINT8 buf[sizeof(FSP_FSCTL_DIR_INFO) + 260 * sizeof(WCHAR)] = {};
  auto* d = (FSP_FSCTL_DIR_INFO*)buf;  // memfs pattern: header + inline name
  size_t namelen = std::min<size_t>(name.size(), 259);
  d->Size = (UINT16)(sizeof(FSP_FSCTL_DIR_INFO) + namelen * sizeof(WCHAR));
  d->FileInfo = info;
  std::memcpy(d->FileNameBuf, name.data(), namelen * sizeof(WCHAR));
  return FspFileSystemFillDirectoryBuffer(dir_buffer, d, result);
}

NTSTATUS SvcReadDirectory(FSP_FILE_SYSTEM*, PVOID file_context, PWSTR, PWSTR marker, PVOID buffer,
                          ULONG length, PULONG bytes_transferred) {
  auto* ctx = (FsContext*)file_context;
  if (!ctx) return STATUS_INVALID_DEVICE_REQUEST;

  NTSTATUS result = STATUS_SUCCESS;
  if (FspFileSystemAcquireDirectoryBuffer(&ctx->dir_buffer, nullptr == marker, &result)) {
    const Resolved& r = ctx->res;
    FSP_FSCTL_FILE_INFO dinfo{};
    Resolved dirres;
    dirres.kind = Resolved::InnerDir;
    fill_file_info(dirres, &dinfo);

    switch (r.kind) {
      case Resolved::Root:
        for (const auto& t : g_vol.ns->tables())
          if (!add_dir_entry(&ctx->dir_buffer, widen(t), dinfo, &result)) break;
        break;
      case Resolved::TableDir:
        for (const auto& o : g_vol.ns->objects(r.table))
          if (!add_dir_entry(&ctx->dir_buffer, widen(o.dir_name), dinfo, &result)) break;
        break;
      case Resolved::ObjectDir:
      case Resolved::InnerDir: {
        auto tree = r.tree;
        for (const auto& c : tree->list(r.inner)) {
          FSP_FSCTL_FILE_INFO info{};
          if (c.is_dir) {
            info = dinfo;
          } else {
            Resolved fr;
            fr.kind = Resolved::File;
            fr.file = *c.file;
            fill_file_info(fr, &info);
          }
          if (!add_dir_entry(&ctx->dir_buffer, widen(c.name), info, &result)) break;
        }
        break;
      }
      case Resolved::File:
        result = STATUS_NOT_A_DIRECTORY;
        break;
    }
    FspFileSystemReleaseDirectoryBuffer(&ctx->dir_buffer);
  }
  if (!NT_SUCCESS(result)) return result;

  FspFileSystemReadDirectoryBuffer(&ctx->dir_buffer, marker, buffer, length, bytes_transferred);
  return STATUS_SUCCESS;
}

// Rename within a record. This is the back half of the Office/CAD save dance
// (write temp, delete original, rename temp into place), so it has to work or
// saves fail at the last step. A temp file that lived on the ephemeral plane
// is PROMOTED to a durable attachment here; cross-record moves are refused.
NTSTATUS SvcRename(FSP_FILE_SYSTEM*, PVOID file_context, PWSTR /*file_name*/, PWSTR new_file_name,
                   BOOLEAN replace_if_exists) {
  auto* ctx = (FsContext*)file_context;
  if (!ctx) return STATUS_INVALID_DEVICE_REQUEST;

  std::string dst = narrow(new_file_name);
  auto parts = split_backslash(dst);
  if (parts.size() < 3) return STATUS_ACCESS_DENIED;
  std::string leaf = parts.back();
  std::string parent = dst.substr(0, dst.find_last_of("\\/"));
  auto pr = resolve(*g_vol.ns, widen(parent).c_str());
  if (!pr || (pr->kind != Resolved::ObjectDir && pr->kind != Resolved::InnerDir))
    return STATUS_OBJECT_PATH_NOT_FOUND;
  if (pr->object.key != ctx->res.object.key || pr->table != ctx->res.table)
    return STATUS_NOT_SAME_DEVICE;   // cross-record move: not in this version
  if (!ctx->is_eph && !g_vol.ns->table_writable(ctx->res.table))
    return STATUS_ACCESS_DENIED;
  std::string new_inner = pr->inner.empty() ? leaf : pr->inner + "/" + leaf;

  // Replacing an existing durable target: soft-delete it first so the name is
  // free (the server keeps it recoverable within retention).
  auto existing = g_vol.ns->live_entry(ctx->res.table, ctx->res.object, new_inner);
  if (existing && !existing->guid.empty()) {
    if (!replace_if_exists) return STATUS_OBJECT_NAME_COLLISION;
    std::string derr;
    g_vol.ns->delete_file(ctx->res.table, ctx->res.object, existing->guid, derr);
  }

  std::string err;
  if (ctx->is_eph) {
    if (is_ephemeral_name(leaf)) {   // temp -> temp: stays on the ephemeral plane
      if (!g_vol.ns->eph_put(ctx->res.table, ctx->res.object, new_inner, ctx->eph_buf, err))
        return STATUS_IO_DEVICE_ERROR;
      g_vol.ns->eph_del(ctx->res.table, ctx->res.object, ctx->res.inner);
      ctx->res.inner = new_inner;
      ctx->dirty = false;
      return STATUS_SUCCESS;
    }
    // Promotion: the app wrote its new content to a temp name and is now
    // moving it into place. Upload the buffer and register a real attachment.
    auto tmp = new_work_path();
    { std::ofstream out(tmp, std::ios::binary); out.write(ctx->eph_buf.data(), (std::streamsize)ctx->eph_buf.size()); }
    std::string hash; uint64_t size = 0;
    bool ok = g_vol.ns->upload(tmp, hash, size, err) &&
              g_vol.ns->add_file(ctx->res.table, ctx->res.object, new_inner, hash,
                                 guess_mimetype(leaf), size, err);
    std::error_code ec; std::filesystem::remove(tmp, ec);
    if (!ok) { error("promote " + new_inner + ": " + err); return STATUS_IO_DEVICE_ERROR; }
    g_vol.ns->eph_del(ctx->res.table, ctx->res.object, ctx->res.inner);
    ctx->is_eph = false;
    ctx->dirty = false;
    ctx->deleted = true;             // the ephemeral original is gone
    ctx->res.inner = new_inner;
    info("promoted " + new_inner + " (" + std::to_string(size) + " bytes)");
    return STATUS_SUCCESS;
  }

  // Durable rename. Flush a live working copy first so the bytes follow the
  // name rather than landing back on the old one at cleanup.
  if (ctx->is_write && ctx->dirty) {
    write_back(ctx);
    ctx->dirty = false;
    ctx->is_write = false;
    discard_work(ctx);
  }
  std::string guid = !ctx->guid.empty() ? ctx->guid
                   : (ctx->res.file ? ctx->res.file->guid : std::string());
  if (guid.empty()) {
    auto live = g_vol.ns->live_entry(ctx->res.table, ctx->res.object, ctx->res.inner);
    if (live) guid = live->guid;
  }
  if (guid.empty()) return STATUS_OBJECT_NAME_NOT_FOUND;
  if (!g_vol.ns->rename_file(ctx->res.table, ctx->res.object, guid, new_inner, err)) {
    error("rename " + ctx->res.inner + " -> " + new_inner + ": " + err);
    return STATUS_ACCESS_DENIED;
  }
  ctx->res.inner = new_inner;
  ctx->guid = guid;
  return STATUS_SUCCESS;
}

HANDLE g_stop_event = nullptr;
BOOL WINAPI ctrl_handler(DWORD) {
  ::SetEvent(g_stop_event);
  return TRUE;
}

}  // namespace

// Create + mount + start the dispatcher. Returns null with `err` set.
// NamespaceService must outlive the returned filesystem.
static FSP_FILE_SYSTEM* mount_volume(const Options& o, NamespaceService& ns, std::string& err) {
  g_vol.ns = &ns;
  g_vol.label = widen(o.volume);

  // Everyone-full-access descriptor; real authorization lives server-side in
  // the per-user session (durable files stay per-file read-only regardless).
  if (!g_vol.sd &&
      !::ConvertStringSecurityDescriptorToSecurityDescriptorW(
          L"O:BAG:BAD:P(A;;FA;;;WD)", SDDL_REVISION_1, &g_vol.sd, &g_vol.sd_size)) {
    err = "failed to build security descriptor";
    return nullptr;
  }

  FSP_FSCTL_VOLUME_PARAMS params{};
  params.Version = sizeof(FSP_FSCTL_VOLUME_PARAMS);
  params.SectorSize = 4096;
  params.SectorsPerAllocationUnit = 1;
  params.VolumeCreationTime = ns.mount_time();
  params.VolumeSerialNumber = (UINT32)(ns.mount_time() / 10000000);
  params.FileInfoTimeout = 1000;  // ms of kernel-side stat caching
  params.CaseSensitiveSearch = 0;
  params.CasePreservedNames = 1;
  params.UnicodeOnDisk = 1;
  params.PersistentAcls = 0;
  // Not ReadOnlyVolume: the ephemeral plane (lock/temp files) is writable.
  // Durable attachments stay read-only per file (attribute + open denial)
  // until the P2 write-back engine.
  params.PostCleanupWhenModifiedOnly = 1;
  params.UmFileContextIsUserContext2 = 1;
  wcscpy_s(params.FileSystemName, L"RecordFS");  // filesystem TYPE, not the label

  static FSP_FILE_SYSTEM_INTERFACE iface = {};
  iface.GetVolumeInfo = SvcGetVolumeInfo;
  iface.GetSecurityByName = SvcGetSecurityByName;
  iface.Create = SvcCreate;
  iface.Overwrite = SvcOverwrite;
  iface.Open = SvcOpen;
  iface.Close = SvcClose;
  iface.Read = SvcRead;
  iface.Write = SvcWrite;
  iface.GetFileInfo = SvcGetFileInfo;
  iface.SetBasicInfo = SvcSetBasicInfo;
  iface.SetFileSize = SvcSetFileSize;
  iface.CanDelete = SvcCanDelete;
  iface.Rename = SvcRename;
  iface.Cleanup = SvcCleanup;
  iface.GetSecurity = SvcGetSecurity;
  iface.ReadDirectory = SvcReadDirectory;

  FSP_FILE_SYSTEM* fs = nullptr;
  NTSTATUS st = FspFileSystemCreate((PWSTR)L"" FSP_FSCTL_DISK_DEVICE_NAME, &params, &iface, &fs);
  if (!NT_SUCCESS(st)) {
    err = "FspFileSystemCreate failed: 0x" + std::to_string((unsigned long)st);
    return nullptr;
  }
  g_vol.fs = fs;

  std::wstring drive = widen(o.drive);
  st = FspFileSystemSetMountPoint(fs, drive.empty() ? nullptr : drive.data());
  if (!NT_SUCCESS(st)) {
    err = "mount point " + o.drive + " unavailable";
    FspFileSystemDelete(fs);
    return nullptr;
  }
  if (o.verbose) {
    // Per-operation NTSTATUS tracing to stderr — the diagnostic of record
    // for "the drive mounts but ops fail" (dir syntax/incorrect-function
    // errors give no clue which callback the driver is unhappy with).
    FspDebugLogSetHandle(::GetStdHandle(STD_ERROR_HANDLE));
    FspFileSystemSetDebugLogF(fs, (UINT32)-1);
  }
  st = FspFileSystemStartDispatcher(fs, 0);
  if (!NT_SUCCESS(st)) {
    err = "failed to start dispatcher";
    FspFileSystemDelete(fs);
    return nullptr;
  }
  info("mounted " + o.drive + " -- server " + o.server);
  return fs;
}

// The notify pump runs for the life of a mount.
static HANDLE g_pump_stop = nullptr;
static std::thread g_pump;

static void start_notify_pump(NamespaceService& ns) {
  g_pump_stop = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  g_pump = std::thread([&ns]() { ns.run_notify_pump(g_pump_stop); });
}

static void stop_notify_pump() {
  if (g_pump_stop) ::SetEvent(g_pump_stop);
  if (g_pump.joinable()) g_pump.join();
  if (g_pump_stop) {
    ::CloseHandle(g_pump_stop);
    g_pump_stop = nullptr;
  }
}

static void unmount_volume(FSP_FILE_SYSTEM* fs) {
  info("unmounting");
  stop_notify_pump();
  FspFileSystemStopDispatcher(fs);
  FspFileSystemDelete(fs);
}

int run_mount(const Options& o) {
  NTSTATUS st = FspLoad(nullptr);
  if (!NT_SUCCESS(st)) {
    error("WinFsp is not installed (FspLoad failed) — install it from https://winfsp.dev/rel/");
    return 1;
  }
  if (o.insecure) warn("--insecure: server certificate NOT verified");
  WsClient ws(o.server, o.insecure);
  if (!ws.login(o.token)) {
    error("login failed: token invalid, expired, or revoked");
    return 1;
  }
  ws.refresh_client_token(o.token);  // slide expiry on every mount
  FileCache cache;
  sweep_stale_parts(cache.root());
  NamespaceService ns(ws, cache);
  std::string err;
  FSP_FILE_SYSTEM* fs = mount_volume(o, ns, err);
  if (!fs) {
    error(err);
    return 1;
  }
  start_notify_pump(ns);
  g_stop_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  ::SetConsoleCtrlHandler(ctrl_handler, TRUE);
  ::WaitForSingleObject(g_stop_event, INFINITE);
  unmount_volume(fs);
  return 0;
}

// The logon agent (worker half — the supervisor lives in main.cpp): wait for
// stored credentials, mount, watch the session's liveness, remount with
// backoff on any failure, and go back to waiting if the token stops
// authenticating (the fix is "log into Scheduler++ again" — never a prompt).
// The desktop client signals this event when it stores fresh credentials.
int run_agent(const Options& base) {
  NTSTATUS st = FspLoad(nullptr);
  if (!NT_SUCCESS(st)) {
    error("agent: WinFsp is not installed — exiting");
    return 0;  // 0 = do not respawn: retrying won't install a driver
  }
  HANDLE cred_event = ::CreateEventW(nullptr, FALSE, FALSE, L"Local\\RecordFS.Credentials");
  g_stop_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  ::SetConsoleCtrlHandler(ctrl_handler, TRUE);
  HANDLE waits[2] = {g_stop_event, cred_event};
  auto stopped = [&](DWORD timeout_ms) {
    return ::WaitForMultipleObjects(2, waits, FALSE, timeout_ms) == WAIT_OBJECT_0;
  };

  int backoff = 5;
  for (;;) {
    Options o = base;
    o.server.clear();
    o.token.clear();  // agent trusts the store only — always re-read it
    if (!resolve_credentials(o)) {
      info("agent: no stored credentials — waiting (log into Scheduler++ once)");
      if (stopped(60000)) return 0;
      continue;
    }
    resolve_drive_settings(o);   // profile first, then machine policy
    try {
      WsClient ws(o.server, o.insecure);
      if (!ws.login(o.token)) {
        warn("agent: token rejected (revoked/expired) — waiting for fresh credentials");
        if (stopped(300000)) return 0;
        continue;
      }
      ws.refresh_client_token(o.token);
      FileCache cache;
      NamespaceService ns(ws, cache);
      std::string err;
      FSP_FILE_SYSTEM* fs = mount_volume(o, ns, err);
      if (!fs) throw std::runtime_error(err);
      start_notify_pump(ns);
      backoff = 5;

      // Supervise: liveness probe every 30 s; token refresh daily.
      int ticks = 0;
      for (;;) {
        if (::WaitForSingleObject(g_stop_event, 30000) == WAIT_OBJECT_0) {
          unmount_volume(fs);
          return 0;
        }
        try {
          ws.request("get_current_stamp", nlohmann::json::object());
          if (++ticks >= 2880) {  // ~daily
            ws.refresh_client_token(o.token);
            ticks = 0;
          }
        } catch (const std::exception& e) {
          warn(std::string("agent: session lost (") + e.what() + ") — remounting");
          break;
        }
      }
      unmount_volume(fs);
    } catch (const std::exception& e) {
      warn(std::string("agent: ") + e.what());
    }
    if (stopped(backoff * 1000)) return 0;
    backoff = std::min(backoff * 2, 60);
  }
}

}  // namespace rfs

#endif  // RECORDFS_HAVE_WINFSP
