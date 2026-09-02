// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Marc Micalizzi
//
// Credential resolution over the shared (MIT) handoff format —
// docs/protocol.md §8, record_mount_credentials.h.
#include "config.h"
#include "log.h"

#include <windows.h>

#include <nlohmann/json.hpp>
#include <record_mount_credentials.h>

#include <cctype>
#include <chrono>
#include <format>

namespace rfs {

bool resolve_credentials(Options& o) {
  if (!o.server.empty() && !o.token.empty()) return true;
  auto stored = record_mount::load(o.profile);
  if (!stored) {
    if (o.server.empty() || o.token.empty()) {
      error("no credentials: pass --server/--token or store a profile with "
            "'recordfs store-token' (profile '" + o.profile + "')");
      return false;
    }
    return true;
  }
  try {
    auto j = nlohmann::json::parse(*stored);
    if (o.server.empty()) o.server = j.value("server", "");
    if (o.token.empty()) o.token = j.value("token", "");
    if (o.drive.empty() && j.contains("drive") && j["drive"].is_string())
      o.drive = j["drive"].get<std::string>();
    if (o.volume.empty() && j.contains("volume") && j["volume"].is_string())
      o.volume = j["volume"].get<std::string>();
  } catch (const std::exception&) {
    error("stored credential profile '" + o.profile + "' is malformed; re-run store-token");
    return false;
  }
  if (o.server.empty() || o.token.empty()) {
    error("stored profile '" + o.profile + "' is incomplete; re-run store-token");
    return false;
  }
  return true;
}

namespace {
// Machine-wide default written by the installer (MSI properties DRIVELETTER /
// VOLUMELABEL). Read from the 64-bit view; absent values simply do not
// contribute.
std::string machine_policy(const wchar_t* value) {
  HKEY key = nullptr;
  if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\RecordFS", 0,
                      KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
    return {};
  wchar_t buf[128];
  DWORD size = sizeof(buf), type = 0;
  std::string out;
  if (::RegQueryValueExW(key, value, nullptr, &type, (LPBYTE)buf, &size) == ERROR_SUCCESS &&
      type == REG_SZ) {
    std::wstring w(buf, size / sizeof(wchar_t));
    while (!w.empty() && w.back() == L'\0') w.pop_back();
    int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    out.resize(n);
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), out.data(), n, nullptr, nullptr);
  }
  ::RegCloseKey(key);
  return out;
}

// "S", "s:", "S:\\" all mean the same drive; WinFsp wants exactly "S:".
std::string normalize_drive(std::string d) {
  while (!d.empty() && (d.back() == '\\' || d.back() == '/')) d.pop_back();
  if (d.size() == 1 && std::isalpha((unsigned char)d[0])) d += ':';
  if (d.size() >= 2) d = d.substr(0, 2);
  if (!d.empty()) d[0] = (char)std::toupper((unsigned char)d[0]);
  return d;
}
}  // namespace

void resolve_drive_settings(Options& o) {
  if (o.drive.empty()) o.drive = machine_policy(L"DriveLetter");
  if (o.volume.empty()) o.volume = machine_policy(L"VolumeLabel");
  if (o.drive.empty()) o.drive = "S:";
  if (o.volume.empty()) o.volume = "Records";
  o.drive = normalize_drive(o.drive);
  if (o.drive.size() != 2 || o.drive[1] != ':') {
    warn("drive '" + o.drive + "' is not a drive letter; using S:");
    o.drive = "S:";
  }
}

bool store_credentials(const Options& o) {
  if (o.server.empty() || o.token.empty()) {
    error("store-token needs --server and --token");
    return false;
  }
  nlohmann::json j;
  j["v"] = 1;
  j["server"] = o.server;
  j["token"] = o.token;
  j["drive"] = o.drive;
  if (!o.volume.empty()) j["volume"] = o.volume;
  auto now = std::chrono::system_clock::now();
  j["created"] = std::format("{:%FT%TZ}", std::chrono::time_point_cast<std::chrono::seconds>(now));
  if (!record_mount::store(j.dump(), o.profile)) {
    error("failed to store credentials (registry/DPAPI)");
    return false;
  }
  info("credentials stored for profile '" + o.profile + "' (user-scoped DPAPI)");
  return true;
}

bool erase_credentials(const Options& o) {
  if (!record_mount::erase(o.profile)) {
    error("failed to erase profile '" + o.profile + "'");
    return false;
  }
  info("profile '" + o.profile + "' erased");
  return true;
}

}  // namespace rfs
