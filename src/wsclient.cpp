// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Marc Micalizzi
#include "wsclient.h"

#include "log.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#include <openssl/x509.h>
#endif

#include <regex>
#include <stdexcept>

namespace beast = boost::beast;
namespace asio = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = asio::ip::tcp;

namespace rfs {

namespace {
#ifdef _WIN32
// OpenSSL does not consult the Windows certificate store on its own; feed the
// machine's ROOT store into the context so real (e.g. Let's Encrypt) chains
// verify without shipping a CA bundle.
void load_windows_roots(ssl::context& ctx) {
  HCERTSTORE store = ::CertOpenSystemStoreW(0, L"ROOT");
  if (!store) return;
  X509_STORE* x509_store = SSL_CTX_get_cert_store(ctx.native_handle());
  PCCERT_CONTEXT cert = nullptr;
  while ((cert = ::CertEnumCertificatesInStore(store, cert)) != nullptr) {
    const unsigned char* der = cert->pbCertEncoded;
    if (X509* x = d2i_X509(nullptr, &der, (long)cert->cbCertEncoded)) {
      X509_STORE_add_cert(x509_store, x);  // dup errors (already present) are fine
      X509_free(x);
    }
  }
  ::CertCloseStore(store, 0);
}
#endif
}  // namespace

struct WsClient::Impl {
  asio::io_context ioc;
  ssl::context ssl_ctx{ssl::context::tls_client};
  std::optional<beast::websocket::stream<beast::tcp_stream>> plain;
  std::optional<beast::websocket::stream<beast::ssl_stream<beast::tcp_stream>>> tls;
  beast::flat_buffer buffer;
  bool open = false;
  bool use_tls = false;
  bool insecure = false;

  std::string host, port, path;

  template <class Ws>
  void ws_setup(Ws& ws, const std::string& hosthdr) {
    beast::get_lowest_layer(ws).expires_never();
    ws.set_option(beast::websocket::stream_base::timeout::suggested(beast::role_type::client));
    ws.handshake(hosthdr, path.empty() ? "/" : path);
  }

  void connect() {
    tcp::resolver resolver(ioc);
    auto results = resolver.resolve(host, port);
    if (use_tls) {
      if (insecure) {
        ssl_ctx.set_verify_mode(ssl::verify_none);
      } else {
        ssl_ctx.set_verify_mode(ssl::verify_peer);
#ifdef _WIN32
        load_windows_roots(ssl_ctx);
#else
        ssl_ctx.set_default_verify_paths();
#endif
      }
      tls.emplace(ioc, ssl_ctx);
      auto& s = *tls;
      beast::get_lowest_layer(s).expires_after(std::chrono::seconds(15));
      auto ep = beast::get_lowest_layer(s).connect(results);
      // SNI + hostname verification (the latter only when verifying).
      if (!SSL_set_tlsext_host_name(s.next_layer().native_handle(), host.c_str()))
        throw std::runtime_error("failed to set SNI hostname");
      if (!insecure && !SSL_set1_host(s.next_layer().native_handle(), host.c_str()))
        throw std::runtime_error("failed to set verification hostname");
      s.next_layer().handshake(ssl::stream_base::client);
      ws_setup(s, host + ":" + std::to_string(ep.port()));
    } else {
      plain.emplace(ioc);
      auto& s = *plain;
      beast::get_lowest_layer(s).expires_after(std::chrono::seconds(15));
      auto ep = beast::get_lowest_layer(s).connect(results);
      ws_setup(s, host + ":" + std::to_string(ep.port()));
    }
    open = true;
  }

  void write(const std::string& payload) {
    if (use_tls)
      tls->write(asio::buffer(payload));
    else
      plain->write(asio::buffer(payload));
  }

  nlohmann::json read() {
    buffer.clear();
    if (use_tls)
      tls->read(buffer);
    else
      plain->read(buffer);
    return nlohmann::json::parse(beast::buffers_to_string(buffer.data()));
  }

  void close() {
    beast::error_code ec;
    if (use_tls && tls)
      tls->close(beast::websocket::close_code::normal, ec);
    else if (plain)
      plain->close(beast::websocket::close_code::normal, ec);
  }
};

WsClient::WsClient(std::string url, bool insecure) : impl_(new Impl), url_(std::move(url)) {
  // ws://host[:port][/path] or wss://... — IPv6 hosts in brackets.
  static const std::regex re(R"(^(wss?)://(\[[^\]]+\]|[^:/]+)(?::(\d+))?(/.*)?$)");
  std::smatch m;
  if (!std::regex_match(url_, m, re))
    throw std::runtime_error("unsupported server url (expected ws://host:port/ or wss://...): " + url_);
  impl_->use_tls = m[1].str() == "wss";
  impl_->insecure = insecure;
  impl_->host = m[2].str();
  if (impl_->host.size() > 1 && impl_->host.front() == '[')
    impl_->host = impl_->host.substr(1, impl_->host.size() - 2);
  impl_->port = m[3].matched ? m[3].str() : "7243";
  impl_->path = m[4].matched ? m[4].str() : "/";
}

WsClient::~WsClient() {
  if (impl_ && impl_->open) impl_->close();
}

bool WsClient::login(const std::string& mount_token) {
  std::lock_guard lock(mutex_);
  impl_->connect();
  nlohmann::json login;
  login["type"] = "login";
  login["id"] = next_id_++;
  login["data"]["session_type"] = "file";
  login["data"]["client_token"] = mount_token;
  impl_->write(login.dump());
  for (;;) {
    auto m = impl_->read();
    std::string type = m.value("type", "");
    if (type == "success") return true;
    if (type == "failure") return false;
    // Anything else pre-login is unexpected but non-fatal; keep reading.
  }
}

nlohmann::json WsClient::request(const std::string& name, nlohmann::json data) {
  std::lock_guard lock(mutex_);
  if (!impl_->open) throw std::runtime_error("not connected");
  uint64_t id = next_id_++;
  nlohmann::json req;
  req["type"] = "request";
  req["request"] = name;
  req["id"] = id;
  req["data"] = std::move(data);
  impl_->write(req.dump());
  for (;;) {
    auto m = impl_->read();
    if (m.value("id", (uint64_t)0) != id) continue;  // stale frame — discard
    if (m.value("type", "") == "error")
      throw std::runtime_error(name + ": " + m.value("details", "server error"));
    return m;
  }
}

nlohmann::json WsClient::list_objects(const std::string& table, size_t start, size_t count) {
  nlohmann::json d;
  d["table"] = table;
  if (start) d["start"] = start;
  if (count) d["count"] = count;
  return request("list_objects", std::move(d));
}

nlohmann::json WsClient::list_files(const std::string& table, const std::string& key_type,
                                    const nlohmann::json& key) {
  nlohmann::json d;
  d["table"] = table;
  d["key_type"] = key_type;
  d["key"] = key;
  return request("list_files", std::move(d));
}

nlohmann::json WsClient::get_file_token(const std::string& attachment_guid) {
  nlohmann::json d;
  d["op"] = "get";
  d["attachment_guid"] = attachment_guid;
  return request("get_file_token", std::move(d));
}

bool WsClient::refresh_client_token(const std::string& token, int ttl_days) {
  nlohmann::json d;
  d["token"] = token;
  d["ttl_days"] = ttl_days;
  return request("refresh_client_token", std::move(d)).value("success", false);
}

nlohmann::json WsClient::get_put_token() {
  nlohmann::json d;
  d["op"] = "put";
  return request("get_file_token", std::move(d));
}

namespace {
// Shared shape for the attachment write family: report the server's own
// reason rather than a generic failure -- the mount surfaces it in the log.
bool write_result(const nlohmann::json& r, std::string* err) {
  if (r.value("success", false)) return true;
  if (err) *err = r.value("error", "refused");
  return false;
}
}  // namespace

bool WsClient::add_attachment(const std::string& table, const std::string& key_type,
                              const nlohmann::json& key, const std::string& filename,
                              const std::string& location, const std::string& mimetype,
                              uint64_t size, std::string* err) {
  nlohmann::json d;
  d["table"] = table;
  d["key_type"] = key_type;
  d["key"] = key;
  d["filename"] = filename;
  d["location"] = location;
  d["mimetype"] = mimetype;
  d["size"] = size;
  return write_result(request("add_attachment", std::move(d)), err);
}

bool WsClient::update_attachment_location(const std::string& guid, const std::string& location,
                                          uint64_t size, std::string* err) {
  nlohmann::json d;
  d["attachment_guid"] = guid;
  d["location"] = location;
  d["size"] = size;
  return write_result(request("update_attachment_location", std::move(d)), err);
}

bool WsClient::rename_attachment(const std::string& guid, const std::string& filename,
                                 std::string* err) {
  nlohmann::json d;
  d["attachment_guid"] = guid;
  d["filename"] = filename;
  return write_result(request("rename_attachment", std::move(d)), err);
}

bool WsClient::delete_attachment(const std::string& table, const std::string& key_type,
                                 const nlohmann::json& key, const std::string& guid,
                                 std::string* err) {
  nlohmann::json d;
  d["table"] = table;
  d["key_type"] = key_type;
  d["key"] = key;
  d["attachment_guid"] = guid;
  return write_result(request("delete_attachment", std::move(d)), err);
}

namespace {
// Standard base64 — the ephemeral plane's inline byte transport.
std::string b64_encode(const std::string& in) {
  static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((in.size() + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 2 < in.size(); i += 3) {
    uint32_t v = (uint8_t)in[i] << 16 | (uint8_t)in[i + 1] << 8 | (uint8_t)in[i + 2];
    out.push_back(tbl[v >> 18]);
    out.push_back(tbl[(v >> 12) & 63]);
    out.push_back(tbl[(v >> 6) & 63]);
    out.push_back(tbl[v & 63]);
  }
  if (i + 1 == in.size()) {
    uint32_t v = (uint8_t)in[i] << 16;
    out.push_back(tbl[v >> 18]);
    out.push_back(tbl[(v >> 12) & 63]);
    out += "==";
  } else if (i + 2 == in.size()) {
    uint32_t v = (uint8_t)in[i] << 16 | (uint8_t)in[i + 1] << 8;
    out.push_back(tbl[v >> 18]);
    out.push_back(tbl[(v >> 12) & 63]);
    out.push_back(tbl[(v >> 6) & 63]);
    out.push_back('=');
  }
  return out;
}

std::optional<std::string> b64_decode(const std::string& in) {
  auto val = [](unsigned char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  std::string out;
  out.reserve(in.size() / 4 * 3);
  int acc = 0, bits = -8;
  for (unsigned char c : in) {
    if (c == '=') break;
    int v = val(c);
    if (v < 0) return std::nullopt;
    acc = (acc << 6) | v;
    bits += 6;
    if (bits >= 0) {
      out.push_back((char)((acc >> bits) & 0xFF));
      bits -= 8;
    }
  }
  return out;
}

nlohmann::json eph_target(const std::string& table, const std::string& key_type,
                          const nlohmann::json& key, const std::string& path) {
  nlohmann::json d;
  d["table"] = table;
  d["key_type"] = key_type;
  d["key"] = key;
  d["path"] = path;
  return d;
}
}  // namespace

bool WsClient::ephemeral_put(const std::string& table, const std::string& key_type,
                             const nlohmann::json& key, const std::string& path,
                             const std::string& bytes, std::string* err) {
  auto d = eph_target(table, key_type, key, path);
  d["data"] = b64_encode(bytes);
  auto r = request("ephemeral_put", std::move(d));
  if (!r.value("success", false)) {
    if (err) *err = r.value("error", "ephemeral_put failed");
    return false;
  }
  return true;
}

std::optional<std::string> WsClient::ephemeral_get(const std::string& table,
                                                   const std::string& key_type,
                                                   const nlohmann::json& key,
                                                   const std::string& path) {
  auto r = request("ephemeral_get", eph_target(table, key_type, key, path));
  if (!r.value("success", false)) return std::nullopt;
  return b64_decode(r.value("data", ""));
}

bool WsClient::ephemeral_delete(const std::string& table, const std::string& key_type,
                                const nlohmann::json& key, const std::string& path) {
  return request("ephemeral_delete", eph_target(table, key_type, key, path))
      .value("success", false);
}

}  // namespace rfs
