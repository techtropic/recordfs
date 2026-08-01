// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Marc Micalizzi
#include "config.h"

#include <cstdio>
#include <cstring>

namespace rfs {

void print_usage() {
  std::fprintf(stderr,
    "recordfs — mount business records as a native Windows drive\n"
    "\n"
    "usage:\n"
    "  recordfs probe       [--server ws[s]://host:port/] [--token T | --profile P]\n"
    "                       [--table workorders] [--key 50049] [-v] [--insecure]\n"
    "  recordfs store-token  --server ws://host:port/ --token T [--drive S:] [--profile P]\n"
    "  recordfs erase-token [--profile P]\n"
    "  recordfs mount       [--profile P] [--drive S:]\n");
}

std::optional<Options> parse_args(int argc, char** argv) {
  Options o;
  if (argc < 2) { print_usage(); return std::nullopt; }
  o.command = argv[1];
  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    auto need = [&](const char* what) -> const char* {
      if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", what); return nullptr; }
      return argv[++i];
    };
    if (a == "--server")      { auto* v = need("--server");  if (!v) return std::nullopt; o.server = v; }
    else if (a == "--token")  { auto* v = need("--token");   if (!v) return std::nullopt; o.token = v; }
    else if (a == "--profile"){ auto* v = need("--profile"); if (!v) return std::nullopt; o.profile = v; }
    else if (a == "--drive")  { auto* v = need("--drive");   if (!v) return std::nullopt; o.drive = v; }
    else if (a == "--table")  { auto* v = need("--table");   if (!v) return std::nullopt; o.table = v; }
    else if (a == "--key")    { auto* v = need("--key");     if (!v) return std::nullopt; o.key = v; }
    else if (a == "-v" || a == "--verbose") { o.verbose = true; }
    else if (a == "--insecure") { o.insecure = true; }
    else { std::fprintf(stderr, "unknown option: %s\n", a.c_str()); print_usage(); return std::nullopt; }
  }
  if (o.command != "probe" && o.command != "store-token" && o.command != "erase-token" &&
      o.command != "mount" && o.command != "help") {
    std::fprintf(stderr, "unknown command: %s\n", o.command.c_str());
    print_usage();
    return std::nullopt;
  }
  return o;
}

}  // namespace rfs
