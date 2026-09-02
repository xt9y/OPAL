#include <opal/net.hpp>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

namespace fs = std::filesystem;

namespace opal {
TlsConn connect_tls_retry(SSL_CTX *ctx,const std::string &host,uint16_t port,int timeout_ms,int retry_ms);
}

static uint16_t free_port() {
    int fd=socket(AF_INET,SOCK_STREAM,0);
    assert(fd>=0);
    sockaddr_in addr{};
    addr.sin_family=AF_INET;
    addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    addr.sin_port=0;
    assert(bind(fd,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))==0);
    socklen_t len=sizeof(addr);
    assert(getsockname(fd,reinterpret_cast<sockaddr*>(&addr),&len)==0);
    uint16_t port=ntohs(addr.sin_port);
    close(fd);
    return port;
}

int main() {
    auto root=fs::temp_directory_path()/"opal-net-retry-test";
    fs::remove_all(root);
    fs::create_directories(root);
    auto cert=(root/"cert.pem").string();
    auto key=(root/"key.pem").string();
    assert(opal::ensure_tls_certificate(cert,key));

    SSL_CTX *server_ctx=opal::server_tls_context(cert,key);
    SSL_CTX *client_ctx=opal::client_tls_context();
    assert(server_ctx&&client_ctx);
    uint16_t port=free_port();

    std::thread server([&]{
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        int listen_fd=opal::listen_tcp(port,"127.0.0.1");
        assert(listen_fd>=0);
        auto c=opal::accept_tls(server_ctx,listen_fd);
        assert(c.ssl);
        assert(opal::tls_write_line(c.ssl,"READY"));
        opal::close_tls(c);
        close(listen_fd);
    });

    auto started=std::chrono::steady_clock::now();
    auto c=opal::connect_tls_retry(client_ctx,"127.0.0.1",port,2500,50);
    auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count();
    assert(c.ssl);
    assert(elapsed>=350);
    assert(elapsed<2500);
    std::string line;
    assert(opal::tls_read_line(c.ssl,line));
    assert(line=="READY");
    opal::close_tls(c);

    server.join();
    SSL_CTX_free(client_ctx);
    SSL_CTX_free(server_ctx);
    fs::remove_all(root);
    std::cout<<"net tests passed\n";
}
