// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Marc Micalizzi
//
// Automatic updates from RecordFS's own public releases.
//
// A small LocalSystem service (`recordfs update-service`, installed by the
// MSI as "RecordFSUpdate") reads the project's release feed -- by default the
// GitHub "latest release" API -- a couple of times a day. When a newer MSI is
// published it downloads it, and installs it only if:
//   - it is Authenticode-signed by the pinned publisher. That signature is
//     what an update is trusted on; the feed and the download host are only
//     transport;
//   - it is RecordFS (the package's UpgradeCode), and the package's OWN
//     ProductVersion is newer than what is installed -- a feed cannot
//     downgrade a machine, whatever its tag says;
//   - its size, and its SHA-256 when the feed publishes one, match.
//
// Applying it has to respect a live drive. Every logged-on user runs an agent
// that holds recordfs.exe and may have documents open on the mount. The
// service raises a machine-wide event; each agent unmounts and exits as soon
// as nothing is open on its drive, and never before. Once no agent is left,
// msiexec runs. MSI stops and restarts this service around the file copy,
// and whichever instance of the service is alive afterwards restarts the
// agents in every session. If files stay open, the update waits for a later
// attempt: nobody's open document is pulled out from under them.
#include "updater.h"

#include "fsdclient.h"  // sha256_hex_of_file
#include "log.h"
#include "version.h"

#include <windows.h>
#include <winhttp.h>
#include <wintrust.h>
#include <softpub.h>
#include <msi.h>
#include <msiquery.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <tlhelp32.h>
#include <sddl.h>
#include <aclapi.h>
#include <share.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <random>
#include <string>
#include <cwctype>
#include <vector>

namespace rfs {

namespace {

constexpr wchar_t kServiceName[] = L"RecordFSUpdate";
constexpr wchar_t kUpdateEvent[] = L"Global\\RecordFS.UpdatePending";
// "Check now": administrators (recordfs update-check --apply) wake the
// service instead of installing from their own console -- a console run from
// the install directory would itself be holding the file being replaced.
constexpr wchar_t kCheckNowEvent[] = L"Global\\RecordFS.UpdateNow";
constexpr wchar_t kPolicyKey[] = L"SOFTWARE\\RecordFS";
constexpr wchar_t kStateKey[] = L"SOFTWARE\\RecordFS\\Updater";
constexpr wchar_t kRunKey[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run";
// The MSI's UpgradeCode: which installed product is RecordFS, and what a
// downloaded package must be before it is installed.
constexpr wchar_t kUpgradeCode[] = L"{B26E3474-CD58-4630-AD22-2D4837D99CFE}";

#ifndef RECORDFS_UPDATE_FEED
#define RECORDFS_UPDATE_FEED "https://api.github.com/repos/techtropic/recordfs/releases/latest"
#endif
// The publisher every installed update must be signed by. A fork that
// publishes its own releases builds with its own identity here.
#ifndef RECORDFS_UPDATE_SIGNER_CN
#define RECORDFS_UPDATE_SIGNER_CN L"Techtropic Inc."
#define RECORDFS_UPDATE_SIGNER_O L"Techtropic Inc."
#define RECORDFS_UPDATE_SIGNER_C L"CA"
#endif

constexpr uint64_t kMaxFeedBytes = 4ull << 20;
constexpr uint64_t kMaxPackageBytes = 512ull << 20;
constexpr auto kQuiesceLimit = std::chrono::minutes(10);

// ---------------------------------------------------------------------------
// small helpers

std::string narrow(const std::wstring& w) {
  if (w.empty()) return {};
  int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
  return s;
}

std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w(n, L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
  return w;
}

std::wstring lower(std::wstring s) {
  for (auto& c : s) c = (wchar_t)::towlower(c);
  return s;
}

std::optional<DWORD> reg_dword(const wchar_t* key, const wchar_t* name) {
  DWORD v = 0, size = sizeof(v);
  if (::RegGetValueW(HKEY_LOCAL_MACHINE, key, name, RRF_RT_REG_DWORD | RRF_SUBKEY_WOW6464KEY,
                     nullptr, &v, &size) == ERROR_SUCCESS)
    return v;
  return std::nullopt;
}

std::wstring reg_string(const wchar_t* key, const wchar_t* name) {
  wchar_t buf[2048];
  DWORD size = sizeof(buf);
  if (::RegGetValueW(HKEY_LOCAL_MACHINE, key, name, RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY,
                     nullptr, buf, &size) == ERROR_SUCCESS)
    return buf;
  return {};
}

void reg_set_string(const wchar_t* name, const std::wstring& value) {
  HKEY k = nullptr;
  if (::RegCreateKeyExW(HKEY_LOCAL_MACHINE, kStateKey, 0, nullptr, 0,
                        KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &k, nullptr) != ERROR_SUCCESS)
    return;
  ::RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)value.c_str(),
                   (DWORD)((value.size() + 1) * sizeof(wchar_t)));
  ::RegCloseKey(k);
}

void reg_delete(const wchar_t* name) {
  HKEY k = nullptr;
  if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, kStateKey, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &k) !=
      ERROR_SUCCESS)
    return;
  ::RegDeleteValueW(k, name);
  ::RegCloseKey(k);
}

std::filesystem::path own_exe() {
  wchar_t buf[MAX_PATH];
  DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
  return std::filesystem::path(std::wstring(buf, n));
}

bool elevated() {
  HANDLE tok = nullptr;
  if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
  TOKEN_ELEVATION e{};
  DWORD n = 0;
  bool yes = ::GetTokenInformation(tok, TokenElevation, &e, sizeof(e), &n) && e.TokenIsElevated;
  ::CloseHandle(tok);
  return yes;
}

// Waits on `stop` for up to `d`; true if it was signalled.
template <class Rep, class Period>
bool stop_within(HANDLE stop, std::chrono::duration<Rep, Period> d) {
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
  return ::WaitForSingleObject(stop, (DWORD)std::clamp<long long>(ms, 0, 0x7fffffff)) ==
         WAIT_OBJECT_0;
}

// ---------------------------------------------------------------------------
// settings (HKLM\SOFTWARE\RecordFS -- machine policy, admin-writable only)

struct Settings {
  bool enabled = true;           // AutoUpdate (the MSI's AUTOUPDATE property)
  std::string feed = RECORDFS_UPDATE_FEED;   // UpdateFeed
  bool allow_unsigned = false;   // UpdateAllowUnsigned -- test machines only
  DWORD interval_h = 12;         // UpdateIntervalHours
};

Settings settings() {
  Settings s;
  if (auto v = reg_dword(kPolicyKey, L"AutoUpdate")) s.enabled = *v != 0;
  else if (auto t = reg_string(kPolicyKey, L"AutoUpdate"); t == L"0") s.enabled = false;
  if (auto f = reg_string(kPolicyKey, L"UpdateFeed"); !f.empty()) s.feed = narrow(f);
  if (auto v = reg_dword(kPolicyKey, L"UpdateAllowUnsigned")) s.allow_unsigned = *v != 0;
  if (auto v = reg_dword(kPolicyKey, L"UpdateIntervalHours")) s.interval_h = std::max<DWORD>(1, *v);
  return s;
}

// ---------------------------------------------------------------------------
// versions

struct Version {
  std::array<unsigned, 4> part{};
  bool valid = false;
  std::string text;
};

// "1.2.3" or "v1.2.3" (up to four parts). Anything with a suffix -- "1.3.0-rc1"
// -- is not a release this updater installs.
Version parse_version(std::string s) {
  Version v;
  v.text = s;
  if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s.erase(0, 1);
  size_t i = 0;
  for (int n = 0; n < 4; ++n) {
    size_t j = i;
    unsigned value = 0;
    while (j < s.size() && s[j] >= '0' && s[j] <= '9') {
      value = value * 10 + (unsigned)(s[j] - '0');
      if (value > 1000000) return v;
      ++j;
    }
    if (j == i) return v;
    v.part[n] = value;
    if (j == s.size()) {
      v.valid = true;
      return v;
    }
    if (s[j] != '.') return v;
    i = j + 1;
  }
  return v;
}

int compare(const Version& a, const Version& b) {
  for (int i = 0; i < 4; ++i) {
    if (a.part[i] < b.part[i]) return -1;
    if (a.part[i] > b.part[i]) return 1;
  }
  return 0;
}

std::string plain(const Version& v) {
  std::string s = std::to_string(v.part[0]) + "." + std::to_string(v.part[1]) + "." +
                  std::to_string(v.part[2]);
  if (v.part[3]) s += "." + std::to_string(v.part[3]);
  return s;
}

// The installed RecordFS, by UpgradeCode (highest if somehow several).
std::optional<Version> installed_version() {
  std::optional<Version> best;
  wchar_t product[39];
  for (DWORD i = 0; ::MsiEnumRelatedProductsW(kUpgradeCode, 0, i, product) == ERROR_SUCCESS; ++i) {
    wchar_t ver[64];
    DWORD n = 64;
    if (::MsiGetProductInfoW(product, INSTALLPROPERTY_VERSIONSTRING, ver, &n) != ERROR_SUCCESS)
      continue;
    Version v = parse_version(narrow(ver));
    if (v.valid && (!best || compare(v, *best) > 0)) best = v;
  }
  return best;
}

// ---------------------------------------------------------------------------
// HTTP (WinHTTP: the machine's proxy settings and Windows' own TLS and roots)

struct Fetched {
  DWORD status = 0;     // 0 = no response at all
  std::string error;
  std::string body;     // when not streaming to a file
  uint64_t bytes = 0;
};

bool loopback(const std::wstring& host) {
  return host == L"localhost" || host == L"127.0.0.1" || host == L"::1" || host == L"[::1]";
}

Fetched http_get(const std::string& url, const wchar_t* accept, HANDLE to_file, uint64_t max_bytes) {
  Fetched r;
  std::wstring wurl = widen(url);
  wchar_t host[256] = {}, path[4096] = {}, extra[4096] = {};
  URL_COMPONENTS uc{};
  uc.dwStructSize = sizeof(uc);
  uc.lpszHostName = host;
  uc.dwHostNameLength = 256;
  uc.lpszUrlPath = path;
  uc.dwUrlPathLength = 4096;
  uc.lpszExtraInfo = extra;
  uc.dwExtraInfoLength = 4096;
  if (!::WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
    r.error = "malformed url";
    return r;
  }
  const bool https = uc.nScheme == INTERNET_SCHEME_HTTPS;
  // Plain HTTP only to this machine (a test feed). Signatures are the trust
  // anchor either way, but there is no reason to fetch in the clear.
  if (!https && !loopback(host)) {
    r.error = "refusing a non-HTTPS url";
    return r;
  }

  std::wstring agent = L"RecordFS-Updater/" + widen(RECORDFS_VERSION);
  HINTERNET session = ::WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) {
    r.error = "WinHttpOpen failed";
    return r;
  }
  ::WinHttpSetTimeouts(session, 15000, 15000, 30000, 60000);
  HINTERNET conn = ::WinHttpConnect(session, host, uc.nPort, 0);
  std::wstring target = std::wstring(path) + extra;
  HINTERNET req = conn ? ::WinHttpOpenRequest(conn, L"GET", target.c_str(), nullptr,
                                              WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              https ? WINHTTP_FLAG_SECURE : 0)
                       : nullptr;
  auto done = [&]() {
    if (req) ::WinHttpCloseHandle(req);
    if (conn) ::WinHttpCloseHandle(conn);
    ::WinHttpCloseHandle(session);
  };
  if (!req) {
    r.error = "could not open a request";
    done();
    return r;
  }
  std::wstring headers = std::wstring(L"Accept: ") + accept +
                         L"\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
  if (!::WinHttpSendRequest(req, headers.c_str(), (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !::WinHttpReceiveResponse(req, nullptr)) {
    r.error = "no response (error " + std::to_string(::GetLastError()) + ")";
    done();
    return r;
  }
  DWORD code = 0, size = sizeof(code);
  ::WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX);
  r.status = code;
  const bool ok = code == 200;
  std::vector<char> buf(64 * 1024);
  for (;;) {
    DWORD avail = 0, got = 0;
    if (!::WinHttpQueryDataAvailable(req, &avail)) {
      r.error = "read failed";
      break;
    }
    if (avail == 0) break;
    if (!::WinHttpReadData(req, buf.data(), std::min<DWORD>(avail, (DWORD)buf.size()), &got) ||
        got == 0)
      break;
    r.bytes += got;
    if (!ok) {  // keep a little of an error body for the log
      if (r.body.size() < 512) r.body.append(buf.data(), std::min<size_t>(got, 512));
      continue;
    }
    if (r.bytes > max_bytes) {
      r.error = "larger than " + std::to_string(max_bytes >> 20) + " MiB; refusing";
      break;
    }
    if (to_file) {
      DWORD wrote = 0;
      if (!::WriteFile(to_file, buf.data(), got, &wrote, nullptr) || wrote != got) {
        r.error = "write failed";
        break;
      }
    } else {
      r.body.append(buf.data(), got);
    }
  }
  done();
  return r;
}

// ---------------------------------------------------------------------------
// the feed (GitHub's release JSON; a mirror or test feed uses the same shape)

struct Release {
  std::string tag;
  Version version;
  std::string asset, url, digest;
  uint64_t size = 0;
};

std::string jstr(const nlohmann::json& j, const char* key) {
  return j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : std::string();
}

std::optional<Release> parse_release(const std::string& body, std::string& why) {
  auto j = nlohmann::json::parse(body, nullptr, false);
  if (j.is_discarded()) {
    why = "the feed did not return JSON";
    return std::nullopt;
  }
  auto published = [](const nlohmann::json& r) {
    return r.is_object() && !(r.contains("draft") && r["draft"] == true) &&
           !(r.contains("prerelease") && r["prerelease"] == true);
  };
  if (j.is_array()) {  // a /releases list: the first published entry
    nlohmann::json pick;
    for (const auto& r : j)
      if (published(r)) {
        pick = r;
        break;
      }
    j = pick;
  }
  if (!published(j)) {
    why = "no published release in the feed";
    return std::nullopt;
  }
  Release out;
  out.tag = jstr(j, "tag_name");
  out.version = parse_version(out.tag);
  if (!out.version.valid) {
    why = "latest release tag '" + out.tag + "' is not a plain version";
    return std::nullopt;
  }
  if (j.contains("assets") && j["assets"].is_array()) {
    for (const auto& a : j["assets"]) {
      std::string name = jstr(a, "name");
      std::string l = name;
      std::transform(l.begin(), l.end(), l.begin(), [](unsigned char c) { return (char)std::tolower(c); });
      if (l.rfind("recordfs", 0) != 0 || l.size() < 4 || l.compare(l.size() - 4, 4, ".msi") != 0)
        continue;
      out.asset = name;
      out.url = jstr(a, "browser_download_url");
      out.digest = jstr(a, "digest");   // "sha256:<hex>" on current GitHub, may be absent
      if (a.contains("size") && a["size"].is_number_unsigned()) out.size = a["size"].get<uint64_t>();
      break;
    }
  }
  if (out.url.empty()) {
    why = "release " + out.tag + " carries no RecordFS .msi";
    return std::nullopt;
  }
  return out;
}

// ---------------------------------------------------------------------------
// verification

struct Signer {
  std::wstring cn, o, c;
};

std::wstring cert_attr(PCCERT_CONTEXT cert, const char* oid) {
  wchar_t buf[256] = {};
  ::CertGetNameStringW(cert, CERT_NAME_ATTR_TYPE, 0, (void*)oid, buf, 256);
  return buf;
}

// Authenticode: a valid signature chaining to a trusted root, and who signed.
bool authenticode(const std::filesystem::path& file, Signer& who, std::string& why) {
  WINTRUST_FILE_INFO fi{};
  fi.cbStruct = sizeof(fi);
  fi.pcwszFilePath = file.c_str();
  WINTRUST_DATA wd{};
  wd.cbStruct = sizeof(wd);
  wd.dwUIChoice = WTD_UI_NONE;
  wd.fdwRevocationChecks = WTD_REVOKE_NONE;
  wd.dwUnionChoice = WTD_CHOICE_FILE;
  wd.pFile = &fi;
  wd.dwStateAction = WTD_STATEACTION_VERIFY;
  GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
  LONG status = ::WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &action, &wd);
  bool ok = status == ERROR_SUCCESS;
  if (ok) {
    CRYPT_PROVIDER_DATA* pd = ::WTHelperProvDataFromStateData(wd.hWVTStateData);
    CRYPT_PROVIDER_SGNR* sg = pd ? ::WTHelperGetProvSignerFromChain(pd, 0, FALSE, 0) : nullptr;
    CRYPT_PROVIDER_CERT* pc = sg ? ::WTHelperGetProvCertFromChain(sg, 0) : nullptr;
    if (pc && pc->pCert) {
      who.cn = cert_attr(pc->pCert, szOID_COMMON_NAME);
      who.o = cert_attr(pc->pCert, szOID_ORGANIZATION_NAME);
      who.c = cert_attr(pc->pCert, szOID_COUNTRY_NAME);
    } else {
      ok = false;
      why = "signature has no signer certificate";
    }
  } else {
    char hex[16];
    sprintf_s(hex, "0x%08lx", (unsigned long)status);
    why = status == TRUST_E_NOSIGNATURE ? "not signed" : std::string("signature invalid (") + hex + ")";
  }
  wd.dwStateAction = WTD_STATEACTION_CLOSE;
  ::WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &action, &wd);
  return ok;
}

std::string msi_property(MSIHANDLE db, const wchar_t* name) {
  std::wstring q = std::wstring(L"SELECT `Value` FROM `Property` WHERE `Property`='") + name + L"'";
  std::string out;
  MSIHANDLE view = 0, rec = 0;
  if (::MsiDatabaseOpenViewW(db, q.c_str(), &view) != ERROR_SUCCESS) return out;
  if (::MsiViewExecute(view, 0) == ERROR_SUCCESS && ::MsiViewFetch(view, &rec) == ERROR_SUCCESS) {
    wchar_t buf[512];
    DWORD n = 512;
    if (::MsiRecordGetStringW(rec, 1, buf, &n) == ERROR_SUCCESS) out = narrow(buf);
    ::MsiCloseHandle(rec);
  }
  ::MsiViewClose(view);
  ::MsiCloseHandle(view);
  return out;
}

// Everything that must be true of a downloaded package before a SYSTEM
// service hands it to msiexec. Returns the package's own version.
std::optional<Version> vet_package(const std::filesystem::path& msi, const Release& rel,
                                   const Settings& s, std::string& why) {
  std::error_code ec;
  uint64_t size = std::filesystem::file_size(msi, ec);
  if (ec || (rel.size && size != rel.size)) {
    why = "size " + std::to_string(size) + " does not match the published " + std::to_string(rel.size);
    return std::nullopt;
  }
  if (rel.digest.rfind("sha256:", 0) == 0) {
    std::string have = "sha256:" + sha256_hex_of_file(msi);
    std::string want = rel.digest;
    std::transform(want.begin(), want.end(), want.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    if (have != want) {
      why = "SHA-256 does not match the published digest";
      return std::nullopt;
    }
  }

  Signer who;
  std::string sig_why;
  if (!authenticode(msi, who, sig_why)) {
    if (!s.allow_unsigned) {
      why = "package " + sig_why;
      return std::nullopt;
    }
    warn("UpdateAllowUnsigned is set: accepting a package that is " + sig_why +
         " -- TEST MACHINES ONLY");
  } else if (who.cn != RECORDFS_UPDATE_SIGNER_CN || who.o != RECORDFS_UPDATE_SIGNER_O ||
             who.c != RECORDFS_UPDATE_SIGNER_C) {
    why = "signed by '" + narrow(who.cn) + "' (" + narrow(who.o) + ", " + narrow(who.c) +
          "), not by the RecordFS publisher";
    return std::nullopt;
  }

  MSIHANDLE db = 0;
  if (::MsiOpenDatabaseW(msi.c_str(), MSIDBOPEN_READONLY, &db) != ERROR_SUCCESS) {
    why = "not a Windows Installer package";
    return std::nullopt;
  }
  std::string upgrade = msi_property(db, L"UpgradeCode");
  Version v = parse_version(msi_property(db, L"ProductVersion"));
  ::MsiCloseHandle(db);
  if (_stricmp(upgrade.c_str(), narrow(kUpgradeCode).c_str()) != 0) {
    why = "not a RecordFS package (UpgradeCode " + upgrade + ")";
    return std::nullopt;
  }
  if (!v.valid) {
    why = "package has no usable ProductVersion";
    return std::nullopt;
  }
  return v;
}

// ---------------------------------------------------------------------------
// staging: <install dir>\updates, writable by SYSTEM and administrators only.
// A SYSTEM service must never write where a user can plant a junction or
// swap a file between verification and msiexec.

bool secure_dir(const std::filesystem::path& dir, std::string& why) {
  PSECURITY_DESCRIPTOR sd = nullptr;
  if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
          L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)", SDDL_REVISION_1, &sd,
          nullptr)) {
    why = "security descriptor";
    return false;
  }
  bool ok = false;
  DWORD attrs = ::GetFileAttributesW(dir.c_str());
  if (attrs == INVALID_FILE_ATTRIBUTES) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
    ok = ::CreateDirectoryW(dir.c_str(), &sa) != 0;
    if (!ok) why = "cannot create " + dir.string();
  } else if (!(attrs & FILE_ATTRIBUTE_DIRECTORY) || (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
    why = dir.string() + " is not a plain directory";
  } else {
    BOOL present = FALSE, defaulted = FALSE;
    PACL dacl = nullptr;
    ::GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted);
    ok = ::SetNamedSecurityInfoW(const_cast<wchar_t*>(dir.c_str()), SE_FILE_OBJECT,
                                 DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                 nullptr, nullptr, dacl, nullptr) == ERROR_SUCCESS;
    if (!ok) why = "cannot secure " + dir.string();
  }
  ::LocalFree(sd);
  return ok;
}

std::filesystem::path stage_dir() { return own_exe().parent_path() / "updates"; }

void prune_stage(const std::filesystem::path& dir) {
  std::error_code ec;
  std::vector<std::filesystem::path> logs;
  for (const auto& de : std::filesystem::directory_iterator(dir, ec)) {
    auto ext = lower(de.path().extension().wstring());
    if (ext == L".msi" || ext == L".part") std::filesystem::remove(de.path(), ec);
    else if (de.path().filename().wstring().rfind(L"install-", 0) == 0) logs.push_back(de.path());
  }
  std::sort(logs.begin(), logs.end(), [](const auto& a, const auto& b) {
    std::error_code e;
    return std::filesystem::last_write_time(a, e) > std::filesystem::last_write_time(b, e);
  });
  for (size_t i = 5; i < logs.size(); ++i) std::filesystem::remove(logs[i], ec);
}

// ---------------------------------------------------------------------------
// agents

// recordfs.exe processes running THIS installation (other copies -- a
// developer's build elsewhere -- do not hold these files and do not count).
std::vector<DWORD> running_agents() {
  std::vector<DWORD> out;
  std::wstring mine = lower(own_exe().wstring());
  HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return out;
  PROCESSENTRY32W pe{};
  pe.dwSize = sizeof(pe);
  for (BOOL more = ::Process32FirstW(snap, &pe); more; more = ::Process32NextW(snap, &pe)) {
    if (pe.th32ProcessID == ::GetCurrentProcessId()) continue;
    if (_wcsicmp(pe.szExeFile, L"recordfs.exe") != 0) continue;
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
    if (!h) continue;
    wchar_t img[MAX_PATH];
    DWORD n = MAX_PATH;
    if (::QueryFullProcessImageNameW(h, 0, img, &n) && lower(std::wstring(img, n)) == mine)
      out.push_back(pe.th32ProcessID);
    ::CloseHandle(h);
  }
  ::CloseHandle(snap);
  return out;
}

// Start the logon agent in every user session, exactly as logon would (the
// machine Run key's own command line). A session that still has an agent
// keeps it: the agent is single-instance per session.
void relaunch_agents() {
  std::wstring cmd = reg_string(kRunKey, L"RecordFS");
  if (cmd.empty()) {
    info("agents not restarted: autostart (the Run key) is not configured on this machine");
    return;
  }
  WTS_SESSION_INFOW* sessions = nullptr;
  DWORD count = 0;
  if (!::WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &count)) return;
  int started = 0;
  for (DWORD i = 0; i < count; ++i) {
    const auto& s = sessions[i];
    if (s.SessionId == 0 || (s.State != WTSActive && s.State != WTSDisconnected)) continue;
    HANDLE token = nullptr;
    if (!::WTSQueryUserToken(s.SessionId, &token)) continue;   // no user, or no privilege
    void* env = nullptr;
    ::CreateEnvironmentBlock(&env, token, FALSE);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.lpDesktop = const_cast<wchar_t*>(L"winsta0\\default");
    PROCESS_INFORMATION pi{};
    std::wstring line = cmd;
    if (::CreateProcessAsUserW(token, nullptr, line.data(), nullptr, nullptr, FALSE,
                               CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, env, nullptr, &si,
                               &pi)) {
      ::CloseHandle(pi.hThread);
      ::CloseHandle(pi.hProcess);
      ++started;
    }
    if (env) ::DestroyEnvironmentBlock(env);
    ::CloseHandle(token);
  }
  ::WTSFreeMemory(sessions);
  info("restarted the agent in " + std::to_string(started) + " session(s)");
}

// The machine-wide "step aside" signal. SYSTEM and administrators control
// it; everyone else may only wait on it, so no user can take other users'
// drives down with it.
HANDLE create_update_event() {
  PSECURITY_DESCRIPTOR sd = nullptr;
  ::ConvertStringSecurityDescriptorToSecurityDescriptorW(
      L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x00100000;;;WD)", SDDL_REVISION_1, &sd, nullptr);
  SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
  HANDLE ev = ::CreateEventW(sd ? &sa : nullptr, TRUE, FALSE, kUpdateEvent);
  if (sd) ::LocalFree(sd);
  // Created fresh, or inherited from a predecessor that died mid-update and
  // whose waiting agents kept it alive: start clear either way.
  if (ev) ::ResetEvent(ev);
  return ev;
}

// Administrators may signal it; nobody else can even open it.
HANDLE create_check_now_event() {
  PSECURITY_DESCRIPTOR sd = nullptr;
  ::ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;SY)(A;;GA;;;BA)",
                                                         SDDL_REVISION_1, &sd, nullptr);
  SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
  HANDLE ev = ::CreateEventW(sd ? &sa : nullptr, FALSE, FALSE, kCheckNowEvent);
  if (sd) ::LocalFree(sd);
  return ev;
}

// Ask every agent to step aside, and wait for all of them to be gone. Returns
// false (event cleared, agents that did leave restarted) when the time runs
// out or `stop` fires.
bool quiesce(HANDLE stop, HANDLE ev) {
  if (running_agents().empty()) return true;
  ::SetEvent(ev);
  info("asking the drive agents to step aside (each waits until nothing is open)");
  auto deadline = std::chrono::steady_clock::now() + kQuiesceLimit;
  for (;;) {
    auto left = running_agents();
    if (left.empty()) return true;
    if (std::chrono::steady_clock::now() > deadline || stop_within(stop, std::chrono::seconds(2))) {
      ::ResetEvent(ev);
      info(std::to_string(left.size()) + " agent(s) still have files open -- update deferred");
      relaunch_agents();
      return false;
    }
  }
}

// ---------------------------------------------------------------------------
// one update cycle

enum class Outcome { UpToDate, Installed, Deferred, Failed, Transient, Stopping };

Outcome run_cycle(HANDLE stop, HANDLE ev, const Settings& s, bool apply,
                  const std::filesystem::path& stage) {
  Version current;
  if (auto inst = installed_version()) {
    current = *inst;
  } else {
    current = parse_version(RECORDFS_VERSION);
    if (!current.valid) {
      error("this build carries no version (" RECORDFS_VERSION ") -- not updating it");
      return Outcome::Failed;
    }
    if (apply) {
      error("RecordFS is not installed through its installer here -- nothing to update");
      return Outcome::Failed;
    }
  }

  auto feed = http_get(s.feed, L"application/vnd.github+json", nullptr, kMaxFeedBytes);
  if (feed.status == 0) {
    warn("update feed unreachable (" + feed.error + "): " + s.feed);
    return Outcome::Transient;
  }
  if (feed.status != 200) {
    std::string hint = feed.status == 404 ? " -- no published release, or the repository is not public"
                     : (feed.status == 403 || feed.status == 429) ? " -- rate limited"
                     : "";
    warn("update feed answered HTTP " + std::to_string(feed.status) + hint);
    return Outcome::Transient;
  }
  std::string why;
  auto rel = parse_release(feed.body, why);
  if (!rel) {
    info("no update: " + why);
    return Outcome::UpToDate;
  }
  if (compare(rel->version, current) <= 0) {
    info("up to date (installed " + plain(current) + ", latest " + rel->tag + ")");
    return Outcome::UpToDate;
  }
  if (apply && narrow(reg_string(kStateKey, L"FailedVersion")) == plain(rel->version)) {
    info("skipping " + rel->tag + ": it failed to install here before; waiting for a newer release");
    return Outcome::UpToDate;
  }
  info("update available: " + plain(current) + " -> " + rel->tag + " (" + rel->asset + ")");

  // Download to a private .part, then vet the final file.
  if (apply && !secure_dir(stage, why)) {
    error("update staging: " + why);
    return Outcome::Failed;
  }
  auto msi = stage / ("RecordFS-" + plain(rel->version) + ".msi");
  auto part = std::filesystem::path(msi.wstring() + L".part");
  HANDLE f = ::CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    error("update staging: cannot write " + part.string());
    return Outcome::Failed;
  }
  auto got = http_get(rel->url, L"application/octet-stream", f, kMaxPackageBytes);
  ::CloseHandle(f);
  std::error_code ec;
  if (got.status != 200 || !got.error.empty()) {
    std::filesystem::remove(part, ec);
    warn("download of " + rel->asset + " failed (HTTP " + std::to_string(got.status) +
         (got.error.empty() ? "" : ", " + got.error) + ")");
    return Outcome::Transient;
  }
  std::filesystem::rename(part, msi, ec);
  auto pkg = vet_package(msi, *rel, s, why);
  if (!pkg) {
    std::filesystem::remove(msi, ec);
    error("refusing " + rel->asset + ": " + why);
    if (apply) reg_set_string(L"FailedVersion", widen(plain(rel->version)));
    return Outcome::Failed;
  }
  if (compare(*pkg, current) <= 0) {
    std::filesystem::remove(msi, ec);
    error("refusing " + rel->asset + ": the package itself is version " + plain(*pkg) +
          ", not newer than the installed " + plain(current));
    if (apply) reg_set_string(L"FailedVersion", widen(plain(rel->version)));
    return Outcome::Failed;
  }
  info("verified " + rel->asset + " (version " + plain(*pkg) + ")");
  if (!apply) return Outcome::UpToDate;

  if (!quiesce(stop, ev)) return Outcome::Deferred;

  // From here on the drives are down; make sure somebody brings them back.
  reg_set_string(L"RelaunchPending", L"1");
  reg_set_string(L"LastAttempt", widen(plain(*pkg)));
  wchar_t sysdir[MAX_PATH];
  ::GetSystemDirectoryW(sysdir, MAX_PATH);
  std::wstring msiexec = std::wstring(sysdir) + L"\\msiexec.exe";
  auto log = stage / ("install-" + plain(*pkg) + ".log");
  std::wstring cmd = L"\"" + msiexec + L"\" /i \"" + msi.wstring() + L"\" /qn /norestart /l*v \"" +
                     log.wstring() + L"\"";
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  info("installing " + plain(*pkg) + " (log: " + log.string() + ")");
  if (!::CreateProcessW(msiexec.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) {
    ::ResetEvent(ev);
    error("could not start msiexec");
    relaunch_agents();
    reg_delete(L"RelaunchPending");
    return Outcome::Failed;
  }
  ::CloseHandle(pi.hThread);
  HANDLE waits[2] = {pi.hProcess, stop};
  DWORD w = ::WaitForMultipleObjects(2, waits, FALSE, INFINITE);
  if (w == WAIT_OBJECT_0 + 1) {
    // The installer is stopping this service to replace its files. It carries
    // on without us; the instance it starts afterwards finishes the update.
    ::CloseHandle(pi.hProcess);
    info("stopping for the installer; the restarted service will finish the update");
    return Outcome::Stopping;
  }
  DWORD code = 1;
  ::GetExitCodeProcess(pi.hProcess, &code);
  ::CloseHandle(pi.hProcess);
  ::ResetEvent(ev);
  Outcome out;
  if (code == 0 || code == 3010) {
    info("installed " + plain(*pkg) + (code == 3010 ? " (takes full effect after a restart)" : ""));
    reg_delete(L"FailedVersion");
    prune_stage(stage);
    out = Outcome::Installed;
  } else if (code == 1618) {
    info("another installation is in progress -- will try again");
    out = Outcome::Deferred;
  } else {
    error("install of " + plain(*pkg) + " failed (msiexec " + std::to_string(code) +
          ") -- see " + log.string());
    reg_set_string(L"FailedVersion", widen(plain(*pkg)));
    out = Outcome::Failed;
  }
  relaunch_agents();
  reg_delete(L"RelaunchPending");
  return out;
}

// An update this service started, and an earlier instance of it could not
// see through (the installer stopped it to replace its files, or it died):
// record how it went and bring the drives back.
void finish_interrupted_update() {
  if (reg_string(kStateKey, L"RelaunchPending").empty()) return;
  std::string attempted = narrow(reg_string(kStateKey, L"LastAttempt"));
  // Judge by THIS binary, not by the installed-product registration: MSI
  // starts the service before it registers the new version, so the registry
  // still names the old one at this point. The service is recordfs.exe
  // itself, so its own version is what is on disk now; a rolled-back install
  // restarts the old binary, which reports the old version.
  Version self = parse_version(RECORDFS_VERSION);
  if (self.valid && plain(self) == attempted) {
    info("update to " + attempted + " complete");
    reg_delete(L"FailedVersion");
    prune_stage(stage_dir());
  } else {
    warn("update to " + attempted + " did not complete (running " RECORDFS_VERSION
         ") -- see the install log in " + stage_dir().string());
    if (!attempted.empty()) reg_set_string(L"FailedVersion", widen(attempted));
  }
  relaunch_agents();
  reg_delete(L"RelaunchPending");
}

// ---------------------------------------------------------------------------
// the service

SERVICE_STATUS_HANDLE g_status_handle = nullptr;
SERVICE_STATUS g_status{};
HANDLE g_service_stop = nullptr;

void report(DWORD state, DWORD wait_hint = 0) {
  static DWORD checkpoint = 1;
  g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  g_status.dwCurrentState = state;
  g_status.dwControlsAccepted =
      state == SERVICE_RUNNING ? (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN) : 0;
  g_status.dwWin32ExitCode = NO_ERROR;
  g_status.dwWaitHint = wait_hint;
  g_status.dwCheckPoint =
      (state == SERVICE_RUNNING || state == SERVICE_STOPPED) ? 0 : checkpoint++;
  ::SetServiceStatus(g_status_handle, &g_status);
}

DWORD WINAPI service_control(DWORD code, DWORD, LPVOID, LPVOID) {
  if (code == SERVICE_CONTROL_STOP || code == SERVICE_CONTROL_SHUTDOWN) {
    report(SERVICE_STOP_PENDING, 10000);
    ::SetEvent(g_service_stop);
    return NO_ERROR;
  }
  return code == SERVICE_CONTROL_INTERROGATE ? NO_ERROR : ERROR_CALL_NOT_IMPLEMENTED;
}

void open_service_log() {
  std::string why;
  auto dir = stage_dir();
  if (!secure_dir(dir, why)) return;   // logs go nowhere rather than somewhere unsafe
  auto path = dir / "updater.log";
  std::error_code ec;
  if (std::filesystem::file_size(path, ec) > (1u << 20)) {
    std::filesystem::rename(path, dir / "updater.log.1", ec);
  }
  if (FILE* f = _wfsopen(path.c_str(), L"a", _SH_DENYNO)) log_target() = f;
}

// Sleep until the next check: false when the service is stopping.
template <class Rep, class Period>
bool sleep_until_next(HANDLE stop, HANDLE now, std::chrono::duration<Rep, Period> d) {
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
  HANDLE waits[2] = {stop, now};
  DWORD w = ::WaitForMultipleObjects(now ? 2 : 1, waits, FALSE,
                                     (DWORD)std::clamp<long long>(ms, 0, 0x7fffffff));
  if (w == WAIT_OBJECT_0 + 1) info("check requested by an administrator");
  return w != WAIT_OBJECT_0;
}

void service_loop(HANDLE stop) {
  HANDLE ev = create_update_event();
  HANDLE now = create_check_now_event();
  info("RecordFS updater " RECORDFS_VERSION " started");
  finish_interrupted_update();

  std::mt19937 rng{std::random_device{}()};
  // Spread first checks out: a building full of PCs booting at 8am should
  // not all ask the feed in the same minute.
  bool running = sleep_until_next(stop, now, std::chrono::seconds(120 + rng() % 600));
  while (running) {
    Settings s = settings();
    std::chrono::minutes next{60};
    if (!s.enabled) {
      info("automatic updates are off (AutoUpdate = 0 in HKLM, SOFTWARE" + std::string(1, (char)92) +
           "RecordFS)");
      next = std::chrono::hours(s.interval_h);
    } else {
      switch (run_cycle(stop, ev, s, /*apply=*/true, stage_dir())) {
        case Outcome::Stopping:
          running = false;
          continue;
        case Outcome::Deferred:  next = std::chrono::minutes(30); break;
        case Outcome::Transient: next = std::chrono::hours(2); break;
        default:
          next = std::chrono::hours(s.interval_h) + std::chrono::minutes(rng() % 60);
          break;
      }
    }
    running = sleep_until_next(stop, now, next);
  }
  if (now) ::CloseHandle(now);
  if (ev) ::CloseHandle(ev);
}

void WINAPI service_main(DWORD, LPWSTR*) {
  g_status_handle = ::RegisterServiceCtrlHandlerExW(kServiceName, service_control, nullptr);
  if (!g_status_handle) return;
  g_service_stop = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  report(SERVICE_START_PENDING, 5000);
  open_service_log();
  report(SERVICE_RUNNING);
  // The work runs on its own thread so a stop is always answered promptly.
  // It notices a stop within seconds everywhere except inside a network call
  // (WinHTTP timeouts reach a minute), and an installer stopping this service
  // must not wait on a download: after a short grace the worker is abandoned
  // and the process exits under it (run_update_service).
  HANDLE worker = ::CreateThread(
      nullptr, 0, [](LPVOID) -> DWORD { service_loop(g_service_stop); return 0; }, nullptr, 0,
      nullptr);
  HANDLE waits[2] = {g_service_stop, worker};
  ::WaitForMultipleObjects(worker ? 2 : 1, waits, FALSE, INFINITE);
  if (worker) {
    ::WaitForSingleObject(worker, 3000);
    ::CloseHandle(worker);
  }
  info("updater stopped");
  report(SERVICE_STOPPED);
}

HANDLE g_console_stop = nullptr;
BOOL WINAPI console_ctrl(DWORD) {
  if (g_console_stop) ::SetEvent(g_console_stop);
  return TRUE;
}

}  // namespace

int run_update_service() {
  SERVICE_TABLE_ENTRYW table[] = {{const_cast<wchar_t*>(kServiceName), service_main},
                                  {nullptr, nullptr}};
  if (!::StartServiceCtrlDispatcherW(table)) {
    error("update-service is run by Windows (service RecordFSUpdate); use update-check from a console");
    return 1;
  }
  // STOPPED is reported; the installer may now replace this binary. Leave at
  // once, without the C++ exit path: a worker abandoned mid-download must
  // not race static destructors (every log line is already flushed).
  std::fflush(nullptr);
  ::ExitProcess(0);
}

int run_update_check(const Options& o) {
  if (o.apply) {
    // Installing is the service's job, always: it runs as SYSTEM, it knows
    // how to take the drives down and bring them back, and it does not hold
    // the files it replaces while the installer runs.
    HANDLE now = ::OpenEventW(EVENT_MODIFY_STATE, FALSE, kCheckNowEvent);
    if (!now) {
      error(::GetLastError() == ERROR_ACCESS_DENIED
                ? "--apply needs an elevated prompt"
                : "the RecordFS updater service is not running (service RecordFSUpdate)");
      return 1;
    }
    ::SetEvent(now);
    ::CloseHandle(now);
    info("asked the updater service to check now; it logs to " +
         (stage_dir() / "updater.log").string());
    return 0;
  }
  // Dry run: what would be installed, fully verified, nothing changed.
  Settings s = settings();
  if (!o.feed.empty()) s.feed = o.feed;
  g_console_stop = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  ::SetConsoleCtrlHandler(console_ctrl, TRUE);
  info("recordfs " RECORDFS_VERSION "; feed " + s.feed);
  auto stage = std::filesystem::temp_directory_path() / "RecordFS-update";
  std::error_code ec;
  std::filesystem::create_directories(stage, ec);
  Outcome out = run_cycle(g_console_stop, nullptr, s, /*apply=*/false, stage);
  std::filesystem::remove_all(stage, ec);
  return out == Outcome::UpToDate ? 0 : 1;
}

// ---------------------------------------------------------------------------
// agent side

UpdateSignal::~UpdateSignal() {
  if (event_) ::CloseHandle((HANDLE)event_);
}

bool UpdateSignal::pending() {
  if (!event_) event_ = ::OpenEventW(SYNCHRONIZE, FALSE, kUpdateEvent);
  if (!event_ || ::WaitForSingleObject((HANDLE)event_, 0) != WAIT_OBJECT_0) return false;
  // Honour it only while the updater is actually there to finish the job.
  bool running = false;
  if (SC_HANDLE scm = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)) {
    if (SC_HANDLE svc = ::OpenServiceW(scm, kServiceName, SERVICE_QUERY_STATUS)) {
      SERVICE_STATUS st{};
      running = ::QueryServiceStatus(svc, &st) && st.dwCurrentState == SERVICE_RUNNING;
      ::CloseServiceHandle(svc);
    }
    ::CloseServiceHandle(scm);
  }
  return running;
}

}  // namespace rfs
