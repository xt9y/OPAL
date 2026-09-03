#pragma once
#include <openssl/ssl.h>
#include <cstdint>
#include <string>
namespace opal {
struct TlsConn { int fd=-1; SSL *ssl=nullptr; };
int listen_tcp(uint16_t port,const std::string &bind_address="0.0.0.0");
int connect_tcp(const std::string &host,uint16_t port);
bool set_tcp_nodelay(int fd);
bool ensure_tls_certificate(const std::string&cert,const std::string&key);
SSL_CTX *server_tls_context(const std::string&cert,const std::string&key);
SSL_CTX *client_tls_context();
TlsConn accept_tls(SSL_CTX *ctx,int listen_fd);
TlsConn accept_tls_timeout(SSL_CTX *ctx,int listen_fd,int timeout_ms);
TlsConn connect_tls(SSL_CTX *ctx,const std::string&host,uint16_t port);
TlsConn connect_tls_retry(SSL_CTX *ctx,const std::string&host,uint16_t port,int timeout_ms=10000,int retry_ms=100);
void close_tls(TlsConn &c);
bool tls_write_all(SSL *ssl,const void *data,size_t size);
bool tls_write_all_timeout(SSL *ssl,const void *data,size_t size,int timeout_ms);
bool tls_write_line(SSL *ssl,const std::string&line);
bool tls_write_line_timeout(SSL *ssl,const std::string&line,int timeout_ms);
// Timed reads retain any partial line across timeout returns and preserve bytes
// after a newline for the next read. This makes short control polling lossless.
bool tls_read_line(SSL *ssl,std::string&line,size_t limit=8192);
bool tls_read_line_timeout(SSL *ssl,std::string&line,int timeout_ms,size_t limit=8192);
bool tls_line_ready(SSL *ssl);
std::string peer_fingerprint(SSL *ssl);
std::string local_fingerprint(SSL *ssl);
std::string primary_ipv4();
}
