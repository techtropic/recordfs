// record_mount_credentials.h — the RecordFS credential-handoff format.
//
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Marc Micalizzi
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS IN THE SOFTWARE.
//
// ---------------------------------------------------------------------------
// This single header is deliberately MIT-licensed and dependency-free so that
// BOTH sides of the credential handoff can vendor it unchanged:
//   - the (possibly proprietary) desktop application that WRITES a mount
//     token at interactive login, and
//   - the GPLv3 RecordFS agent that READS it to mount the drive.
// It is the reference implementation of docs/protocol.md §8. Keep the two
// vendored copies identical.
//
// Format (Windows):
//   HKCU\Software\RecordFS\mounts\<profile>   (profile default: "default")
//     value "blob" (REG_BINARY) =
//       CryptProtectData(user scope, entropy "RecordFS.credential.v1")
//       over UTF-8 JSON:
//   { "v":1, "server":"ws://host:7243/", "token":"...", "drive":"S:",
//     "user":"alice", "created":"2026-07-21T14:03:22Z" }
//
// Per-user by construction: HKCU + user-scoped DPAPI. Readers treat missing/
// undecryptable/malformed blobs as "no credentials", never as fatal errors.
// This header stores and returns the raw JSON string; JSON parsing is the
// caller's business (both sides already carry a JSON library).
// ---------------------------------------------------------------------------

#pragma once
#if defined(_WIN32)

#include <windows.h>
#include <dpapi.h>
#include <optional>
#include <string>
#include <vector>

#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "advapi32.lib")

namespace record_mount {

inline constexpr wchar_t kRegRoot[] = L"Software\\RecordFS\\mounts";
inline constexpr wchar_t kRegValue[] = L"blob";
inline constexpr char kEntropy[] = "RecordFS.credential.v1";

namespace detail {
inline std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w(n, L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
  return w;
}
inline std::string narrow(const std::wstring& w) {
  if (w.empty()) return {};
  int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
  return s;
}
inline DATA_BLOB entropy_blob() {
  // Not a secret — a domain separator so unrelated DPAPI blobs can't be
  // replayed into this format.
  static const std::string e = kEntropy;
  return DATA_BLOB{(DWORD)e.size(), (BYTE*)e.data()};
}
inline std::wstring key_path(const std::string& profile) {
  return std::wstring(kRegRoot) + L"\\" + widen(profile.empty() ? "default" : profile);
}
}  // namespace detail

// Encrypt `json` for the current user and store it under the profile.
// Returns false on any registry/DPAPI failure.
inline bool store(const std::string& json, const std::string& profile = "default") {
  DATA_BLOB in{(DWORD)json.size(), (BYTE*)json.data()};
  DATA_BLOB entropy = detail::entropy_blob();
  DATA_BLOB out{};
  if (!::CryptProtectData(&in, L"RecordFS mount credential", &entropy, nullptr,
                          nullptr, 0, &out))
    return false;
  HKEY key = nullptr;
  bool ok = false;
  if (::RegCreateKeyExW(HKEY_CURRENT_USER, detail::key_path(profile).c_str(), 0,
                        nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
    ok = ::RegSetValueExW(key, kRegValue, 0, REG_BINARY, out.pbData,
                          out.cbData) == ERROR_SUCCESS;
    ::RegCloseKey(key);
  }
  ::LocalFree(out.pbData);
  return ok;
}

// Load and decrypt the profile's credential JSON. nullopt = no usable
// credentials (absent, wrong user, or corrupt — indistinguishable on purpose).
inline std::optional<std::string> load(const std::string& profile = "default") {
  HKEY key = nullptr;
  if (::RegOpenKeyExW(HKEY_CURRENT_USER, detail::key_path(profile).c_str(), 0,
                      KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
    return std::nullopt;
  DWORD type = 0, size = 0;
  std::vector<BYTE> buf;
  if (::RegQueryValueExW(key, kRegValue, nullptr, &type, nullptr, &size) == ERROR_SUCCESS &&
      type == REG_BINARY && size > 0) {
    buf.resize(size);
    if (::RegQueryValueExW(key, kRegValue, nullptr, &type, buf.data(), &size) != ERROR_SUCCESS)
      buf.clear();
  }
  ::RegCloseKey(key);
  if (buf.empty()) return std::nullopt;

  DATA_BLOB in{(DWORD)buf.size(), buf.data()};
  DATA_BLOB entropy = detail::entropy_blob();
  DATA_BLOB out{};
  if (!::CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr, 0, &out))
    return std::nullopt;
  std::string json((char*)out.pbData, out.cbData);
  ::SecureZeroMemory(out.pbData, out.cbData);
  ::LocalFree(out.pbData);
  return json;
}

// Remove the profile's stored credential. True if it is gone afterwards
// (including "was never there").
inline bool erase(const std::string& profile = "default") {
  LSTATUS st = ::RegDeleteKeyW(HKEY_CURRENT_USER, detail::key_path(profile).c_str());
  return st == ERROR_SUCCESS || st == ERROR_FILE_NOT_FOUND;
}

// Profiles with a stored blob (names only; blobs are not decrypted).
inline std::vector<std::string> profiles() {
  std::vector<std::string> out;
  HKEY key = nullptr;
  if (::RegOpenKeyExW(HKEY_CURRENT_USER, kRegRoot, 0, KEY_ENUMERATE_SUB_KEYS, &key) != ERROR_SUCCESS)
    return out;
  for (DWORD i = 0;; ++i) {
    wchar_t name[256];
    DWORD len = 256;
    if (::RegEnumKeyExW(key, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
      break;
    out.push_back(detail::narrow(std::wstring(name, len)));
  }
  ::RegCloseKey(key);
  return out;
}

}  // namespace record_mount

#endif  // _WIN32
