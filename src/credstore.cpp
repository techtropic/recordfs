// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Marc Micalizzi
//
// Credential resolution over the shared (MIT) handoff format —
// docs/protocol.md §8, record_mount_credentials.h.
#include "config.h"
#include "log.h"

#include <nlohmann/json.hpp>
#include <record_mount_credentials.h>

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
    if (j.contains("drive") && j["drive"].is_string()) o.drive = j["drive"];
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
