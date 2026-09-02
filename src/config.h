// SPDX-License-Identifier: MIT
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
  std::string drive;    // resolved by resolve_drive_settings()
  std::string volume;   // volume label shown in Explorer
  std::string table;              // probe: focus table (default workorders)
  std::string key;                // probe: focus record key
  bool verbose = false;
  bool insecure = false;          // wss: skip certificate verification (dev only)
};

// Parse argv. On error prints usage and returns nullopt.
std::optional<Options> parse_args(int argc, char** argv);

// Fill server/token/drive from the stored DPAPI profile when not given on the
// command line. Returns false if credentials are needed but absent.
bool resolve_credentials(Options& o);

// Persist / erase the credential profile (store-token, erase-token).
bool store_credentials(const Options& o);
bool erase_credentials(const Options& o);

// Drive letter and volume label, in precedence order:
//   1. --drive / --volume on the command line
//   2. the per-user credential profile (what the desktop wrote at login)
//   3. machine policy: HKLM\\SOFTWARE\\RecordFS DriveLetter / VolumeLabel
//      (set by the installer, including from its command line)
//   4. built-in defaults (S: and "Records")
// Call after resolve_credentials so a stored profile can contribute.
void resolve_drive_settings(Options& o);

void print_usage();

}  // namespace rfs
