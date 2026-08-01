// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Marc Micalizzi
//
// The WinFsp adapter: presents the record namespace as a read-only drive.
//
//   S:\<table>\<key> - <display>\<attachment path...>
//
// P1 scope: browse + open (hydrate-on-open, reads served from the local
// content-addressed cache). The volume is marked read-only at the driver
// level; the write-back engine arrives in a later phase behind this same
// namespace service.
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
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
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

struct ObjectEntry {
  std::string key;       // record key (identity)
  std::string dir_name;  // "<key> - <display>"
  bool numeric = false;
  long long id = 0;
};

class NamespaceService {
public:
  NamespaceService(WsClient& ws, FileCache& cache, std::vector<std::string> tables)
      : ws_(ws), cache_(cache), tables_(std::move(tables)), mount_time_(now_filetime()) {}

  uint64_t mount_time() const { return mount_time_; }
  const std::vector<std::string>& tables() const { return tables_; }

  bool is_table(const std::string& name) const {
    for (const auto& t : tables_)
      if (lower_ascii(t) == lower_ascii(name)) return true;
    return false;
  }

  // Table-dir listing (one list_objects, TTL-cached).
  std::vector<ObjectEntry> objects(const std::string& table) {
    std::lock_guard lock(mutex_);
    auto& c = objects_[lower_ascii(table)];
    auto now = std::chrono::steady_clock::now();
    if (c.fetched + std::chrono::seconds(30) < now || c.entries.empty()) {
      auto r = ws_.list_objects(table);
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
    return std::nullopt;
  }

  // A record's file tree (TTL-cached per record).
  std::shared_ptr<const FileTree> files(const std::string& table, const ObjectEntry& obj) {
    std::string ck = lower_ascii(table) + "\x1f" + obj.key;
    std::lock_guard lock(mutex_);
    auto& c = trees_[ck];
    auto now = std::chrono::steady_clock::now();
    if (!c.tree || c.fetched + std::chrono::seconds(10) < now) {
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
      c.tree = std::move(tree);
      c.fetched = now;
    }
    return c.tree;
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

  // Hydrate-on-open: cache hit or mint + pinned fetch.
  std::optional<std::filesystem::path> hydrate(const FileEntry& e, std::string& err) {
    if (!e.location.starts_with("sha256:")) {
      err = "not daemon-backed";
      return std::nullopt;
    }
    auto dest = cache_.path_for(e.location);
    if (cache_.present(e.location)) return dest;
    auto grant = ws_.get_file_token(e.guid);
    if (!grant.contains("result")) {
      err = grant.value("error", "grant refused");
      return std::nullopt;
    }
    auto res = grant["result"];
    if (res.value("mode", "") != "direct") {
      err = "direct byte plane unavailable";
      return std::nullopt;
    }
    auto fetch = fetch_blob(res.value("urls", std::vector<std::string>{}),
                            res.value("fingerprint", ""), e.location,
                            res.value("token", ""), dest);
    if (!fetch.ok) {
      err = fetch.error;
      return std::nullopt;
    }
    return dest;
  }

private:
  struct ObjectsCache {
    std::vector<ObjectEntry> entries;
    std::map<std::string, size_t> by_name;
    std::chrono::steady_clock::time_point fetched{};
  };
  struct TreeCache {
    std::shared_ptr<const FileTree> tree;
    std::chrono::steady_clock::time_point fetched{};
  };

  WsClient& ws_;
  FileCache& cache_;
  std::vector<std::string> tables_;
  uint64_t mount_time_;
  std::mutex mutex_;
  std::map<std::string, ObjectsCache> objects_;
  std::map<std::string, TreeCache> trees_;
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
  HANDLE local = INVALID_HANDLE_VALUE;  // hydrated cache file (durable File)
  PVOID dir_buffer = nullptr;
  // Ephemeral file state: bytes live in this buffer between open and close;
  // close (or flush) writes them back to the server plane.
  bool is_eph = false;
  std::string eph_buf;
  bool dirty = false;
  bool deleted = false;
};

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
  PSECURITY_DESCRIPTOR sd = nullptr;
  ULONG sd_size = 0;
};
Volume g_vol;

void fill_file_info(const Resolved& r, FSP_FSCTL_FILE_INFO* info) {
  std::memset(info, 0, sizeof(*info));
  uint64_t t0 = g_vol.ns->mount_time();
  info->CreationTime = info->LastAccessTime = info->LastWriteTime = info->ChangeTime = t0;
  if (r.kind == Resolved::File && r.file) {
    // Durable attachments are read-only until the P2 write-back engine;
    // ephemeral (lock/temp) files are ordinary writable files.
    info->FileAttributes =
        r.file->ephemeral ? FILE_ATTRIBUTE_ARCHIVE : FILE_ATTRIBUTE_READONLY;
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
  const wchar_t label[] = L"RecordFS";
  std::memcpy(v->VolumeLabel, label, sizeof(label));
  v->VolumeLabelLength = (UINT16)(sizeof(label) - sizeof(wchar_t));
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

// New files: only ephemeral (lock/temp pattern) names inside a record are
// creatable — that is the D2 plane. Durable creation is the P2/P3 write-back
// engine. The Create/Open/Overwrite trio must all exist or the WinFsp Create
// dispatcher refuses every open with IoStatus=c0000010.
NTSTATUS SvcCreate(FSP_FILE_SYSTEM*, PWSTR file_name, UINT32 create_options, UINT32,
                   UINT32, PSECURITY_DESCRIPTOR, UINT64, PVOID* file_context,
                   FSP_FSCTL_FILE_INFO* info) {
  if (create_options & FILE_DIRECTORY_FILE) return STATUS_MEDIA_WRITE_PROTECTED;

  // Resolve the parent (everything but the leaf) — it must be inside a record.
  std::string full = narrow(file_name);
  auto parts = split_backslash(full);
  if (parts.size() < 3) return STATUS_MEDIA_WRITE_PROTECTED;
  std::string leaf = parts.back();
  if (!is_ephemeral_name(leaf)) return STATUS_MEDIA_WRITE_PROTECTED;
  std::string parent = full.substr(0, full.find_last_of("\\/"));
  auto pr = resolve(*g_vol.ns, widen(parent).c_str());
  if (!pr || (pr->kind != Resolved::ObjectDir && pr->kind != Resolved::InnerDir))
    return STATUS_OBJECT_PATH_NOT_FOUND;

  auto ctx = std::make_unique<FsContext>();
  ctx->res = *pr;
  ctx->res.kind = Resolved::File;
  ctx->res.inner = pr->inner.empty() ? leaf : pr->inner + "/" + leaf;
  ctx->is_eph = true;
  ctx->dirty = true;  // an empty create still registers on close
  // Register server-side immediately so the file exists for other handles
  // and other clients from the moment of creation.
  std::string err;
  if (!g_vol.ns->eph_put(ctx->res.table, ctx->res.object, ctx->res.inner, "", err)) {
    warn("ephemeral create " + ctx->res.inner + ": " + err);
    return STATUS_IO_DEVICE_ERROR;
  }
  fill_eph_info(ctx.get(), info);
  *file_context = ctx.release();
  return STATUS_SUCCESS;
}

NTSTATUS SvcOverwrite(FSP_FILE_SYSTEM*, PVOID file_context, UINT32, BOOLEAN, UINT64,
                      FSP_FSCTL_FILE_INFO* info) {
  auto* ctx = (FsContext*)file_context;
  if (!ctx || !ctx->is_eph) return STATUS_MEDIA_WRITE_PROTECTED;
  ctx->eph_buf.clear();
  ctx->dirty = true;
  fill_eph_info(ctx, info);
  return STATUS_SUCCESS;
}

NTSTATUS SvcWrite(FSP_FILE_SYSTEM*, PVOID file_context, PVOID buffer, UINT64 offset,
                  ULONG length, BOOLEAN write_to_eof, BOOLEAN constrained,
                  PULONG bytes_transferred, FSP_FSCTL_FILE_INFO* info) {
  auto* ctx = (FsContext*)file_context;
  if (!ctx || !ctx->is_eph) return STATUS_MEDIA_WRITE_PROTECTED;
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
  if (!ctx || !ctx->is_eph) return STATUS_MEDIA_WRITE_PROTECTED;
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
  // Apps set attributes/times on their lock files; accept and ignore —
  // ephemeral metadata is not worth a server round-trip.
  if (ctx->is_eph) {
    fill_eph_info(ctx, info);
    return STATUS_SUCCESS;
  }
  return STATUS_ACCESS_DENIED;
}

NTSTATUS SvcCanDelete(FSP_FILE_SYSTEM*, PVOID file_context, PWSTR) {
  auto* ctx = (FsContext*)file_context;
  if (ctx && ctx->is_eph) return STATUS_SUCCESS;
  return STATUS_ACCESS_DENIED;
}

VOID SvcCleanup(FSP_FILE_SYSTEM*, PVOID file_context, PWSTR, ULONG flags) {
  auto* ctx = (FsContext*)file_context;
  if (!ctx || !ctx->is_eph) return;
  if ((flags & FspCleanupDelete) && !ctx->deleted) {
    ctx->deleted = true;
    if (!g_vol.ns->eph_del(ctx->res.table, ctx->res.object, ctx->res.inner))
      warn("ephemeral delete " + ctx->res.inner + " failed");
    return;
  }
  // Cleanup = the app's handle close, and it runs synchronously with it —
  // flush HERE, not in Close: the kernel defers the final Close IRP, so a
  // write-back parked there loses the race against the very next open
  // (write → reopen → read saw the pre-write bytes).
  if (ctx->dirty && !ctx->deleted) {
    std::string err;
    if (g_vol.ns->eph_put(ctx->res.table, ctx->res.object, ctx->res.inner, ctx->eph_buf, err))
      ctx->dirty = false;
    else
      warn("ephemeral write-back " + ctx->res.inner + ": " + err);
  }
}

NTSTATUS SvcOpen(FSP_FILE_SYSTEM*, PWSTR file_name, UINT32, UINT32 granted_access,
                 PVOID* file_context, FSP_FSCTL_FILE_INFO* info) {
  auto r = resolve(*g_vol.ns, file_name);
  if (!r) return STATUS_OBJECT_NAME_NOT_FOUND;

  auto ctx = std::make_unique<FsContext>();
  ctx->res = *r;
  if (r->kind == Resolved::File && r->file->ephemeral) {
    // Ephemeral: pull the bytes into the handle's buffer; close writes back.
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
    // Durable attachments are read-only until the P2 write-back engine.
    if (granted_access & (FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE))
      return STATUS_ACCESS_DENIED;
    std::string err;
    auto local = g_vol.ns->hydrate(*r->file, err);
    if (!local) {
      warn("hydrate " + r->file->path + ": " + err);
      return STATUS_IO_DEVICE_ERROR;
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
  if (ctx->is_eph && ctx->dirty && !ctx->deleted) {
    // Close cannot report errors — best-effort write-back with a log trail.
    std::string err;
    if (!g_vol.ns->eph_put(ctx->res.table, ctx->res.object, ctx->res.inner, ctx->eph_buf, err))
      warn("ephemeral write-back " + ctx->res.inner + ": " + err);
  }
  if (ctx->local != INVALID_HANDLE_VALUE) ::CloseHandle(ctx->local);
  if (ctx->dir_buffer) FspFileSystemDeleteDirectoryBuffer(&ctx->dir_buffer);
  delete ctx;
}

NTSTATUS SvcRead(FSP_FILE_SYSTEM*, PVOID file_context, PVOID buffer, UINT64 offset, ULONG length,
                 PULONG bytes_transferred) {
  auto* ctx = (FsContext*)file_context;
  if (ctx && ctx->is_eph) {
    if (offset >= ctx->eph_buf.size()) {
      *bytes_transferred = 0;
      return STATUS_END_OF_FILE;
    }
    size_t n = std::min<size_t>(length, ctx->eph_buf.size() - (size_t)offset);
    std::memcpy(buffer, ctx->eph_buf.data() + offset, n);
    *bytes_transferred = (ULONG)n;
    return STATUS_SUCCESS;
  }
  if (!ctx || ctx->local == INVALID_HANDLE_VALUE) return STATUS_INVALID_DEVICE_REQUEST;
  OVERLAPPED ov{};
  ov.Offset = (DWORD)(offset & 0xFFFFFFFF);
  ov.OffsetHigh = (DWORD)(offset >> 32);
  DWORD n = 0;
  if (!::ReadFile(ctx->local, buffer, length, &n, &ov)) {
    if (::GetLastError() == ERROR_HANDLE_EOF) {
      *bytes_transferred = 0;
      return STATUS_END_OF_FILE;
    }
    return STATUS_IO_DEVICE_ERROR;
  }
  if (n == 0) {
    *bytes_transferred = 0;
    return STATUS_END_OF_FILE;
  }
  *bytes_transferred = n;
  return STATUS_SUCCESS;
}

NTSTATUS SvcGetFileInfo(FSP_FILE_SYSTEM*, PVOID file_context, FSP_FSCTL_FILE_INFO* info) {
  auto* ctx = (FsContext*)file_context;
  if (!ctx) return STATUS_INVALID_DEVICE_REQUEST;
  if (ctx->is_eph)
    fill_eph_info(ctx, info);
  else
    fill_file_info(ctx->res, info);
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

HANDLE g_stop_event = nullptr;
BOOL WINAPI ctrl_handler(DWORD) {
  ::SetEvent(g_stop_event);
  return TRUE;
}

}  // namespace

int run_mount(const Options& o) {
  NTSTATUS st = FspLoad(nullptr);
  if (!NT_SUCCESS(st)) {
    error("WinFsp is not installed (FspLoad failed) — install it from https://winfsp.dev/rel/");
    return 1;
  }

  WsClient ws(o.server, o.insecure);
  if (o.insecure) warn("--insecure: server certificate NOT verified");
  if (!ws.login(o.token)) {
    error("login failed: token invalid, expired, or revoked");
    return 1;
  }
  ws.refresh_client_token(o.token);  // slide expiry on every mount
  FileCache cache;
  std::string table = o.table.empty() ? "workorders" : o.table;
  NamespaceService ns(ws, cache, {table});
  g_vol.ns = &ns;

  // Everyone-full-access descriptor; real authorization lives server-side in
  // the per-user session (and the volume is read-only in P1 regardless).
  if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
          L"O:BAG:BAD:P(A;;FA;;;WD)", SDDL_REVISION_1, &g_vol.sd, &g_vol.sd_size)) {
    error("failed to build security descriptor");
    return 1;
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
  wcscpy_s(params.FileSystemName, L"RecordFS");

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
  iface.Cleanup = SvcCleanup;
  iface.GetSecurity = SvcGetSecurity;
  iface.ReadDirectory = SvcReadDirectory;

  FSP_FILE_SYSTEM* fs = nullptr;
  st = FspFileSystemCreate((PWSTR)L"" FSP_FSCTL_DISK_DEVICE_NAME, &params, &iface, &fs);
  if (!NT_SUCCESS(st)) {
    error("FspFileSystemCreate failed: 0x" + std::to_string((unsigned long)st));
    return 1;
  }
  g_vol.fs = fs;

  std::wstring drive = widen(o.drive);
  st = FspFileSystemSetMountPoint(fs, drive.empty() ? nullptr : drive.data());
  if (!NT_SUCCESS(st)) {
    error("mount point " + o.drive + " unavailable");
    FspFileSystemDelete(fs);
    return 1;
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
    error("failed to start dispatcher");
    FspFileSystemDelete(fs);
    return 1;
  }

  info("mounted " + o.drive + "\\ (read-only) — table " + table + ", server " + o.server);
  g_stop_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  ::SetConsoleCtrlHandler(ctrl_handler, TRUE);
  ::WaitForSingleObject(g_stop_event, INFINITE);

  info("unmounting");
  FspFileSystemStopDispatcher(fs);
  FspFileSystemDelete(fs);
  ::LocalFree(g_vol.sd);
  return 0;
}

}  // namespace rfs

#endif  // RECORDFS_HAVE_WINFSP
