// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Marc Micalizzi
#include "fsdclient.h"

#include "log.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <openssl/evp.h>
#include <openssl/x509.h>

#include <chrono>
#include <fstream>
#include <regex>

namespace beast = boost::beast;
namespace http = beast::http;
namespace asio = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = asio::ip::tcp;

namespace rfs {

namespace {

std::string to_hex(const unsigned char* data, size_t len) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    out.push_back(digits[data[i] >> 4]);
    out.push_back(digits[data[i] & 0xF]);
  }
  return out;
}

// Lowercase-hex SHA-256 of the peer certificate's DER encoding — the
// daemon's identity (docs/protocol.md §6.2 step 1).
std::string peer_cert_fingerprint(SSL* ssl) {
  X509* cert = SSL_get1_peer_certificate(ssl);
  if (!cert) return {};
  unsigned char* der = nullptr;
  int len = i2d_X509(cert, &der);
  std::string fp;
  if (len > 0) {
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int mdlen = 0;
    if (EVP_Digest(der, (size_t)len, md, &mdlen, EVP_sha256(), nullptr))
      fp = to_hex(md, mdlen);
  }
  OPENSSL_free(der);
  X509_free(cert);
  return fp;
}

struct UrlParts {
  std::string host, port;
};

bool parse_https_url(const std::string& url, UrlParts& out) {
  static const std::regex re(R"(^https://(\[[^\]]+\]|[^:/]+)(?::(\d+))?/?$)");
  std::smatch m;
  if (!std::regex_match(url, m, re)) return false;
  out.host = m[1].str();
  if (out.host.size() > 1 && out.host.front() == '[')
    out.host = out.host.substr(1, out.host.size() - 2);
  out.port = m[2].matched ? m[2].str() : "443";
  return true;
}

bool fetch_one(const UrlParts& u, const std::string& fingerprint, const std::string& hash,
               const std::string& bearer, const std::filesystem::path& dest,
               uint64_t& size_out, std::string& err) {
  try {
    asio::io_context ioc;
    ssl::context ctx(ssl::context::tls_client);
    ctx.set_verify_mode(ssl::verify_none);  // identity = the pin below, not a CA chain

    beast::ssl_stream<beast::tcp_stream> stream(ioc, ctx);
    tcp::resolver resolver(ioc);
    auto results = resolver.resolve(u.host, u.port);
    beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(10));
    beast::get_lowest_layer(stream).connect(results);
    stream.handshake(ssl::stream_base::client);

    std::string fp = peer_cert_fingerprint(stream.native_handle());
    if (fp != fingerprint) {
      err = "fingerprint mismatch (got " + fp.substr(0, 12) + "..., pinned " +
            fingerprint.substr(0, 12) + "...)";
      return false;
    }

    http::request<http::empty_body> req(http::verb::get, "/files/" + hash, 11);
    req.set(http::field::host, u.host);
    req.set(http::field::authorization, "Bearer " + bearer);
    req.set(http::field::user_agent, "recordfs/0.1");
    beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(120));
    http::write(stream, req);

    // Stream the body straight to a temp file next to dest.
    std::filesystem::path tmp = dest;
    tmp += ".part";
    beast::flat_buffer buffer;
    http::response_parser<http::file_body> parser;
    parser.body_limit(1ull << 40);
    beast::error_code ec;
    parser.get().body().open(tmp.string().c_str(), beast::file_mode::write, ec);
    if (ec) { err = "open " + tmp.string() + ": " + ec.message(); return false; }
    http::read(stream, buffer, parser);
    parser.get().body().close();

    auto status = parser.get().result_int();
    if (status != 200) {
      std::filesystem::remove(tmp);
      err = "HTTP " + std::to_string(status);
      return false;
    }

    // Verify content addressing end-to-end before the file becomes visible.
    std::string got = sha256_hex_of_file(tmp);
    std::string want = hash.starts_with("sha256:") ? hash.substr(7) : hash;
    if (got != want) {
      std::filesystem::remove(tmp);
      err = "content hash mismatch after download";
      return false;
    }
    size_out = std::filesystem::file_size(tmp);
    std::filesystem::rename(tmp, dest);

    beast::error_code sec;
    stream.shutdown(sec);  // best-effort TLS close
    return true;
  } catch (const std::exception& e) {
    err = e.what();
    return false;
  }
}

}  // namespace

std::string sha256_hex_of_file(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return {};
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  if (!ctx) return {};
  EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
  char buf[65536];
  while (in.read(buf, sizeof(buf)) || in.gcount())
    EVP_DigestUpdate(ctx, buf, (size_t)in.gcount());
  unsigned char md[EVP_MAX_MD_SIZE];
  unsigned int mdlen = 0;
  EVP_DigestFinal_ex(ctx, md, &mdlen);
  EVP_MD_CTX_free(ctx);
  return to_hex(md, mdlen);
}

FetchResult fetch_blob(const std::vector<std::string>& urls, const std::string& fingerprint,
                       const std::string& hash, const std::string& bearer,
                       const std::filesystem::path& dest) {
  FetchResult r;
  if (urls.empty()) { r.error = "no advertised urls"; return r; }
  if (fingerprint.size() != 64) { r.error = "no pinned fingerprint"; return r; }
  std::filesystem::create_directories(dest.parent_path());
  auto t0 = std::chrono::steady_clock::now();
  for (const auto& url : urls) {
    UrlParts u;
    if (!parse_https_url(url, u)) {
      r.error += (r.error.empty() ? "" : "; ") + url + ": unsupported url";
      continue;
    }
    std::string err;
    if (fetch_one(u, fingerprint, hash, bearer, dest, r.size, err)) {
      r.ok = true;
      r.url_used = url;
      r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      return r;
    }
    r.error += (r.error.empty() ? "" : "; ") + url + ": " + err;
  }
  return r;
}

}  // namespace rfs
