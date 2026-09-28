// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Marc Micalizzi
#pragma once
#include "config.h"

namespace rfs {

// `recordfs update-service` -- the entry point when Windows starts the
// RecordFSUpdate service (installed by the MSI). Not for console use.
int run_update_service();

// `recordfs update-check [--feed URL] [--apply]` -- one check in the
// foreground: report what the feed offers and verify it; --apply (elevated)
// also installs it, exactly as the service would.
int run_update_check(const Options& o);

// Agent side: the updater asks every mount to step aside before it replaces
// recordfs.exe. Mounts honour it only while nothing is open on the drive, and
// only while the updater service is actually running -- a signal left behind
// by an updater that died mid-update must never take drives down for good.
class UpdateSignal {
public:
  UpdateSignal() = default;
  ~UpdateSignal();
  UpdateSignal(const UpdateSignal&) = delete;
  UpdateSignal& operator=(const UpdateSignal&) = delete;

  bool pending();

private:
  void* event_ = nullptr;
};

}  // namespace rfs
