// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Marc Micalizzi
#include "wsclient.h"

#include "log.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

#include <regex>
#include <stdexcept>

namespace beast = boost::beast;
namespace asio = boost::asio;
using tcp = asio::ip::tcp;

namespace rfs {

struct WsClient::Impl {
  asio::io_context ioc;
  beast::websocket::stream<beast::tcp_stream> ws{ioc};
  beast::flat_buffer buffer;
  bool open = false;

  std::string host, port, path;

  void connect() {
    tcp::resolver resolver(ioc);
    auto results = resolver.resolve(host, port);
    beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(15));
    auto ep = beast::get_lowest_layer(ws).connect(results);
    std::string hosthdr = host + ":" + std::to_string(ep.port());
    // WS handshakes/reads get no beast timeout by default; set the suggested
    // client timeouts (idle ping keeps NATs open on long-lived mounts).
    beast::get_lowest_layer(ws).expires_never();
    ws.set_option(beast::websocket::stream_base::timeout::suggested(beast::role_type::client));
    ws.handshake(hosthdr, path.empty() ? "/" : path);
    open = true;
  }

  nlohmann::json roundtrip_read() {
    buffer.clear();
    ws.read(buffer);
    return nlohmann::json::parse(beast::buffers_to_string(buffer.data()));
  }
};

WsClient::WsClient(std::string url) : impl_(new Impl), url_(std::move(url)) {
  // ws://host[:port][/path] — IPv6 hosts in brackets.
  static const std::regex re(R"(^ws://(\[[^\]]+\]|[^:/]+)(?::(\d+))?(/.*)?$)");
  std::smatch m;
  if (!std::regex_match(url_, m, re))
    throw std::runtime_error("unsupported server url (expected ws://host:port/): " + url_);
  impl_->host = m[1].str();
  if (impl_->host.size() > 1 && impl_->host.front() == '[')
    impl_->host = impl_->host.substr(1, impl_->host.size() - 2);
  impl_->port = m[2].matched ? m[2].str() : "7243";
  impl_->path = m[3].matched ? m[3].str() : "/";
}

WsClient::~WsClient() {
  if (impl_ && impl_->open) {
    beast::error_code ec;
    impl_->ws.close(beast::websocket::close_code::normal, ec);
  }
}

bool WsClient::login(const std::string& mount_token) {
  std::lock_guard lock(mutex_);
  impl_->connect();
  nlohmann::json login;
  login["type"] = "login";
  login["id"] = next_id_++;
  login["data"]["session_type"] = "file";
  login["data"]["client_token"] = mount_token;
  impl_->ws.write(asio::buffer(login.dump()));
  for (;;) {
    auto m = impl_->roundtrip_read();
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
  impl_->ws.write(asio::buffer(req.dump()));
  for (;;) {
    auto m = impl_->roundtrip_read();
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

}  // namespace rfs
