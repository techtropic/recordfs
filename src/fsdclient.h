// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Marc Micalizzi
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rfs {

// Byte-plane client — docs/protocol.md §6.2. Fetches content-addressed blobs
// from the file-storage daemon over TLS verified BY FINGERPRINT PIN (SHA-256
// of the peer's DER certificate must equal the server-vouched value; CA
// chain and hostname are irrelevant). Downloaded bytes are SHA-256-verified
// against the requested hash before the destination file appears.
struct FetchResult {
  bool ok = false;
  std::string error;      // per-url failures, joined
  std::string url_used;   // which advertised url succeeded
  uint64_t size = 0;
  double ms = 0;          // transfer time
};

// Upload result for the write-back engine. `hash` is the content id the
// daemon stored the bytes under -- the value an attachment row is re-pointed
// at (docs/protocol.md 6.1 op=put).
struct PutResult {
  bool ok = false;
  std::string error;
  std::string hash;      // "sha256:<hex>"
  uint64_t size = 0;
  std::string url_used;
  double ms = 0;
};

PutResult put_blob(const std::vector<std::string>& urls,
                   const std::string& fingerprint,
                   const std::string& bearer,
                   const std::filesystem::path& src);

FetchResult fetch_blob(const std::vector<std::string>& urls,
                       const std::string& fingerprint,   // 64 lowercase hex
                       const std::string& hash,          // "sha256:<hex>"
                       const std::string& bearer,        // grant token
                       const std::filesystem::path& dest);

// SHA-256 of a file's contents as lowercase hex ("" on open failure).
std::string sha256_hex_of_file(const std::filesystem::path& p);

}  // namespace rfs
