// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Marc Micalizzi
#include "config.h"
#include "log.h"
#include "probe.h"

#include <cstdio>

#ifdef RECORDFS_HAVE_WINFSP
namespace rfs {
int run_mount(const Options& o);  // winfsp_fs.cpp
}
#endif

int main(int argc, char** argv) {
  auto opts = rfs::parse_args(argc, argv);
  if (!opts) return 2;
  rfs::Options& o = *opts;

  try {
    if (o.command == "help") {
      rfs::print_usage();
      return 0;
    }
    if (o.command == "store-token") return rfs::store_credentials(o) ? 0 : 1;
    if (o.command == "erase-token") return rfs::erase_credentials(o) ? 0 : 1;
    if (o.command == "probe") {
      if (!rfs::resolve_credentials(o)) return 1;
      return rfs::run_probe(o);
    }
    if (o.command == "mount") {
#ifdef RECORDFS_HAVE_WINFSP
      if (!rfs::resolve_credentials(o)) return 1;
      return rfs::run_mount(o);
#else
      rfs::error("this build has no mount support (WinFsp SDK was not present at build time)");
      return 1;
#endif
    }
  } catch (const std::exception& e) {
    rfs::error(e.what());
    return 1;
  }
  return 2;
}
