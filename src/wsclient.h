// SPDX-License-Identifier: MIT
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
  // Tables that have at least one attachment and that this user may read
  // (protocol docs section 4.0). The mount root is discovered, not configured.
  nlohmann::json list_tables();
  nlohmann::json list_objects(const std::string& table, size_t start = 0, size_t count = 0);
  nlohmann::json list_files(const std::string& table, const std::string& key_type,
                            const nlohmann::json& key);
  nlohmann::json get_file_token(const std::string& attachment_guid);
  bool refresh_client_token(const std::string& token, int ttl_days = 30);

  // Byte-plane upload grant (protocol §6.1, op=put).
  nlohmann::json get_put_token();

  // Attachment metadata writes (protocol §3 write family). Each returns false
  // and fills `err` on refusal (permission, unknown row, bad location).
  bool add_attachment(const std::string& table, const std::string& key_type,
                      const nlohmann::json& key, const std::string& filename,
                      const std::string& location, const std::string& mimetype,
                      uint64_t size, std::string* err = nullptr);
  bool update_attachment_location(const std::string& guid, const std::string& location,
                                  uint64_t size, std::string* err = nullptr);
  bool rename_attachment(const std::string& guid, const std::string& filename,
                         std::string* err = nullptr);
  bool delete_attachment(const std::string& table, const std::string& key_type,
                         const nlohmann::json& key, const std::string& guid,
                         std::string* err = nullptr);

  // Ephemeral plane (protocol §4.3/D2): raw bytes in/out, base64 on the wire.
  bool ephemeral_put(const std::string& table, const std::string& key_type,
                     const nlohmann::json& key, const std::string& path,
                     const std::string& bytes, std::string* err = nullptr);
  std::optional<std::string> ephemeral_get(const std::string& table, const std::string& key_type,
                                           const nlohmann::json& key, const std::string& path);
  bool ephemeral_delete(const std::string& table, const std::string& key_type,
                        const nlohmann::json& key, const std::string& path);

  // File leases (protocol §4.5): cross-machine share modes. `access` and
  // `share` use the FILE_SHARE_* bit layout (1 read, 2 write, 4 delete).
  // Returns the response frame: {success, granted, holder?}. Throws like
  // request() -- a 403 means the server predates leases.
  nlohmann::json lease_acquire(const std::string& table, const std::string& key_type,
                               const nlohmann::json& key, const std::string& path,
                               const std::string& open_id, int access, int share,
                               const std::string& machine);
  bool lease_release(const std::string& open_id);
  nlohmann::json lease_renew();

  const std::string& url() const { return url_; }

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::string url_;
  std::mutex mutex_;
  uint64_t next_id_ = 1;
};

}  // namespace rfs
