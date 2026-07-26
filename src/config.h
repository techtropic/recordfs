// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Marc Micalizzi
#pragma once
#include <optional>
#include <string>
#include <vector>

namespace rfs {

struct Options {
  std::string command;            // probe | store-token | erase-token | mount | help
  std::string server;             // ws://host:port/
  std::string token;              // mount token (plaintext)
  std::string profile = "default";
  std::string drive = "S:";
  std::string table;              // probe: focus table (default workorders)
  std::string key;                // probe: focus record key
  bool verbose = false;
};

// Parse argv. On error prints usage and returns nullopt.
std::optional<Options> parse_args(int argc, char** argv);

// Fill server/token/drive from the stored DPAPI profile when not given on the
// command line. Returns false if credentials are needed but absent.
bool resolve_credentials(Options& o);

// Persist / erase the credential profile (store-token, erase-token).
bool store_credentials(const Options& o);
bool erase_credentials(const Options& o);

void print_usage();

}  // namespace rfs
