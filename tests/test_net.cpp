#include <opal/net.hpp>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

namespace fs = std::filesystem;

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

    uint16_t blocked_port=free_port();
    std::thread blocked_server([&]{
        int listen_fd=opal::listen_tcp(blocked_port,"127.0.0.1");
        assert(listen_fd>=0);
        auto peer=opal::accept_tls(server_ctx,listen_fd);
        assert(peer.ssl);
        std::this_thread::sleep_for(std::chrono::seconds(2));
        opal::close_tls(peer);
        close(listen_fd);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto blocked=opal::connect_tls(client_ctx,"127.0.0.1",blocked_port);
    assert(blocked.ssl);
    int sndbuf=4096;
    setsockopt(blocked.fd,SOL_SOCKET,SO_SNDBUF,&sndbuf,sizeof(sndbuf));
    std::vector<unsigned char> payload(32*1024*1024,0x5a);
    started=std::chrono::steady_clock::now();
    assert(!opal::tls_write_all_timeout(blocked.ssl,payload.data(),payload.size(),250));
    elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count();
    assert(elapsed<1500);
    opal::close_tls(blocked);
    blocked_server.join();

    SSL_CTX_free(client_ctx);
    SSL_CTX_free(server_ctx);
    fs::remove_all(root);
    std::cout<<"net tests passed\n";
}
