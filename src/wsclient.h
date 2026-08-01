// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Marc Micalizzi
#pragma once
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace rfs {

// File-session client over WebSocket — docs/protocol.md §§1-5.
//
// Synchronous, one connection, thread-safe: concurrent callers are
// mutex-serialized per request/response round-trip (file sessions receive no
// unsolicited frames, so strict request→response pairing holds).
//
// Transports: ws:// (plain) and wss:// (TLS). wss verifies the server
// certificate against the Windows ROOT store with hostname checking;
// `insecure` disables verification (dev/self-signed only — never in
// production; the connection still encrypts but proves nothing).
class WsClient {
public:
  explicit WsClient(std::string url, bool insecure = false);  // ws[s]://host:port/
  ~WsClient();
  WsClient(const WsClient&) = delete;
  WsClient& operator=(const WsClient&) = delete;

  // Connect + file-session login (docs/protocol.md §2). Throws on transport
  // errors; returns false on login rejection (bad/expired/revoked token).
  bool login(const std::string& mount_token);

  // Issue an allowlisted request and return its response frame.
  // Throws std::runtime_error on transport failure or a 403 allowlist error.
  nlohmann::json request(const std::string& name, nlohmann::json data);

  // Convenience wrappers (shapes per docs/protocol.md §4/§6.1).
  nlohmann::json list_objects(const std::string& table, size_t start = 0, size_t count = 0);
  nlohmann::json list_files(const std::string& table, const std::string& key_type,
                            const nlohmann::json& key);
  nlohmann::json get_file_token(const std::string& attachment_guid);
  bool refresh_client_token(const std::string& token, int ttl_days = 30);

  // Ephemeral plane (protocol §4.3/D2): raw bytes in/out, base64 on the wire.
  bool ephemeral_put(const std::string& table, const std::string& key_type,
                     const nlohmann::json& key, const std::string& path,
                     const std::string& bytes, std::string* err = nullptr);
  std::optional<std::string> ephemeral_get(const std::string& table, const std::string& key_type,
                                           const nlohmann::json& key, const std::string& path);
  bool ephemeral_delete(const std::string& table, const std::string& key_type,
                        const nlohmann::json& key, const std::string& path);

  const std::string& url() const { return url_; }

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::string url_;
  std::mutex mutex_;
  uint64_t next_id_ = 1;
};

}  // namespace rfs
