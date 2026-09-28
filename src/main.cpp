// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Marc Micalizzi
#include "config.h"
#include "log.h"
#include "probe.h"
#include "updater.h"
#include "version.h"

#include <windows.h>
#include <share.h>

#include <cstdio>
#include <string>

#ifdef RECORDFS_HAVE_WINFSP
namespace rfs {
int run_mount(const Options& o);  // winfsp_fs.cpp
int run_agent(const Options& o);  // winfsp_fs.cpp (agent worker)
}
#endif

namespace {

// The exe is a WINDOWS-subsystem binary so the logon agent never flashes a
// console; CLI use re-attaches to the invoking terminal's console instead,
// and the headless agent worker logs to %LOCALAPPDATA%\RecordFS\agent.log.
// A stream already redirected to a file or pipe (`recordfs version > v.txt`,
// PowerShell capturing output) is kept: re-pointing it at the console would
// silently throw the caller's capture away.
bool redirected(DWORD which) {
  HANDLE h = ::GetStdHandle(which);
  if (!h || h == INVALID_HANDLE_VALUE) return false;
  DWORD type = ::GetFileType(h);
  return type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE;
}

void attach_parent_console(bool headless_worker) {
  const bool out_redirected = redirected(STD_OUTPUT_HANDLE);
  const bool err_redirected = redirected(STD_ERROR_HANDLE);
  if (!headless_worker && ::AttachConsole(ATTACH_PARENT_PROCESS)) {
    FILE* f;
    if (!out_redirected) freopen_s(&f, "CONOUT$", "w", stdout);
    if (!err_redirected) freopen_s(&f, "CONOUT$", "w", stderr);
    freopen_s(&f, "CONIN$", "r", stdin);
    return;
  }
  if (headless_worker) {
    char* base = nullptr;
    size_t len = 0;
    if (_dupenv_s(&base, &len, "LOCALAPPDATA") == 0 && base) {
      std::string dir = std::string(base) + "\\RecordFS";
      free(base);
      ::CreateDirectoryA(dir.c_str(), nullptr);
      if (FILE* f = _fsopen((dir + "\\agent.log").c_str(), "a", _SH_DENYNO))
        rfs::log_target() = f;
    }
  }
}

std::wstring own_path() {
  wchar_t buf[MAX_PATH];
  ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
  return buf;
}

// `recordfs agent` — the Run-key entry point. Supervises an `agent-run`
// child: respawn with a short delay on crash/nonzero exit, stop for good on
// clean exit (logoff, deliberate stop, WinFsp absent). All child output goes
// to %LOCALAPPDATA%\RecordFS\agent.log.
int run_supervisor(const rfs::Options& o) {
  // One agent per session. The Run key starts one at every logon, and the
  // updater restarts agents after installing; a second agent would only
  // fight the first for the drive letter.
  HANDLE single = ::CreateMutexW(nullptr, TRUE, L"Local\\RecordFS.Agent");
  if (single && ::GetLastError() == ERROR_ALREADY_EXISTS) return 0;
  std::string logdir;
  {
    char* base = nullptr;
    size_t len = 0;
    if (_dupenv_s(&base, &len, "LOCALAPPDATA") == 0 && base) {
      logdir = std::string(base) + "\\RecordFS";
      free(base);
      ::CreateDirectoryA(logdir.c_str(), nullptr);
    }
  }
  for (;;) {
    // The worker opens its own agent.log (attach_parent_console handles the
    // redirect) — handle inheritance into a GUI-subsystem CRT is unreliable.
    // Forward what the worker cannot rediscover. Drive and volume normally
    // come from machine policy or the stored profile, but an explicit switch
    // on the agent command line must still win.
    std::wstring cmd = L"\"" + own_path() + L"\" agent-run --profile " +
                       std::wstring(o.profile.begin(), o.profile.end());
    if (!o.drive.empty())
      cmd += L" --drive " + std::wstring(o.drive.begin(), o.drive.end());
    if (!o.volume.empty())
      cmd += L" --volume \"" + std::wstring(o.volume.begin(), o.volume.end()) + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                          nullptr, nullptr, &si, &pi))
      return 1;
    ::WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    ::GetExitCodeProcess(pi.hProcess, &code);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(pi.hThread);
    if (code == 0) return 0;  // clean stop — do not respawn
    if (code == rfs::kExitForUpdate) return 0;  // the updater restarts us afterwards
    ::Sleep(5000);
  }
}

}  // namespace

int main(int argc, char** argv) {
  attach_parent_console(argc > 1 && std::string(argv[1]) == "agent-run");
  auto opts = rfs::parse_args(argc, argv);
  if (!opts) return 2;
  rfs::Options& o = *opts;

  try {
    if (o.command == "help") {
      rfs::print_usage();
      return 0;
    }
    if (o.command == "version") {
      std::printf("recordfs %s\n", RECORDFS_VERSION);
      return 0;
    }
    if (o.command == "update-service") return rfs::run_update_service();
    if (o.command == "update-check") return rfs::run_update_check(o);
    if (o.command == "store-token") {
      // Record whatever the caller asked for, filled in from machine policy.
      rfs::resolve_drive_settings(o);
      return rfs::store_credentials(o) ? 0 : 1;
    }
    if (o.command == "erase-token") return rfs::erase_credentials(o) ? 0 : 1;
    if (o.command == "probe") {
      if (!rfs::resolve_credentials(o)) return 1;
      return rfs::run_probe(o);
    }
    if (o.command == "agent") return run_supervisor(o);
    if (o.command == "agent-run" || o.command == "mount") {
#ifdef RECORDFS_HAVE_WINFSP
      if (o.command == "agent-run") return rfs::run_agent(o);
      if (!rfs::resolve_credentials(o)) return 1;
      // AFTER credentials: a stored profile may carry drive/volume, and it
      // only wins over machine policy if the defaults have not been applied.
      rfs::resolve_drive_settings(o);
      return rfs::run_mount(o);
#else
      rfs::error("this build has no mount support (WinFsp SDK was not present at build time)");
      return o.command == "agent-run" ? 0 : 1;  // 0: supervisor must not respawn
#endif
    }
  } catch (const std::exception& e) {
    rfs::error(e.what());
    return 1;
  }
  return 2;
}
