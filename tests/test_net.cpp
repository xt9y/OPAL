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
#include <fcntl.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/stat.h>

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

static void assert_cloexec(int fd){
    int flags=fcntl(fd,F_GETFD,0);
    assert(flags>=0);
    assert((flags&FD_CLOEXEC)!=0);
}

int main() {
    auto root=fs::temp_directory_path()/"opal-net-retry-test";
    fs::remove_all(root);
    fs::create_directories(root);
    auto cert=(root/"cert.pem").string();
    auto key=(root/"key.pem").string();
    assert(opal::ensure_tls_certificate(cert,key));
    assert(chmod(key.c_str(),0644)==0);
    assert(opal::ensure_tls_certificate(cert,key));
    struct stat key_stat{};assert(stat(key.c_str(),&key_stat)==0);
    assert((key_stat.st_mode&0777)==0600);

    SSL_CTX *server_ctx=opal::server_tls_context(cert,key);
    SSL_CTX *client_ctx=opal::client_tls_context();
    assert(server_ctx&&client_ctx);
    uint16_t port=free_port();

    std::thread server([&]{
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        int listen_fd=opal::listen_tcp(port,"127.0.0.1");
        assert(listen_fd>=0);
        assert_cloexec(listen_fd);
        auto c=opal::accept_tls(server_ctx,listen_fd);
        assert(c.ssl);
        assert_cloexec(c.fd);
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
    assert_cloexec(c.fd);
    int nodelay=0;socklen_t nodelay_len=sizeof(nodelay);
    assert(getsockopt(c.fd,IPPROTO_TCP,TCP_NODELAY,&nodelay,&nodelay_len)==0);
    assert(nodelay==1);
    std::string line;
    assert(opal::tls_read_line(c.ssl,line));
    assert(line=="READY");
    opal::close_tls(c);
    server.join();

    // Short polling must never consume and lose a partial control line. It
    // must also preserve a second complete line received in the same TLS read.
    uint16_t fragmented_port=free_port();
    std::thread fragmented_server([&]{
        int listen_fd=opal::listen_tcp(fragmented_port,"127.0.0.1");
        assert(listen_fd>=0);
        auto peer=opal::accept_tls(server_ctx,listen_fd);
        assert(peer.ssl);
        const std::string first="UDP_SEL";
        assert(opal::tls_write_all(peer.ssl,first.data(),first.size()));
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        const std::string rest="ECTED 7 127.0.0.1 4567\nPONG\n";
        assert(opal::tls_write_all(peer.ssl,rest.data(),rest.size()));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        opal::close_tls(peer);
        close(listen_fd);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto fragmented=opal::connect_tls(client_ctx,"127.0.0.1",fragmented_port);
    assert(fragmented.ssl);
    line.clear();
    assert(!opal::tls_read_line_timeout(fragmented.ssl,line,30));
    assert(opal::tls_read_line_timeout(fragmented.ssl,line,1000));
    assert(line=="UDP_SELECTED 7 127.0.0.1 4567");
    assert(opal::tls_line_ready(fragmented.ssl));
    assert(opal::tls_read_line_timeout(fragmented.ssl,line,100));
    assert(line=="PONG");
    assert(!opal::tls_line_ready(fragmented.ssl));
    opal::close_tls(fragmented);
    fragmented_server.join();

    uint16_t delayed_port=free_port();
    std::thread delayed_server([&]{
        int listen_fd=opal::listen_tcp(delayed_port,"127.0.0.1");
        assert(listen_fd>=0);
        auto peer=opal::accept_tls(server_ctx,listen_fd);
        assert(peer.ssl);
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        assert(opal::tls_write_line(peer.ssl,"LATE"));
        opal::close_tls(peer);
        close(listen_fd);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto delayed=opal::connect_tls(client_ctx,"127.0.0.1",delayed_port);
    assert(delayed.ssl);
    started=std::chrono::steady_clock::now();
    line.clear();
    assert(opal::tls_read_line_timeout(delayed.ssl,line,5000));
    elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count();
    assert(line=="LATE");
    assert(elapsed>=1000);
    assert(elapsed<5000);
    opal::close_tls(delayed);
    delayed_server.join();

    uint16_t silent_port=free_port();
    std::thread silent_server([&]{
        int listen_fd=opal::listen_tcp(silent_port,"127.0.0.1");
        assert(listen_fd>=0);
        auto peer=opal::accept_tls(server_ctx,listen_fd);
        assert(peer.ssl);
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        opal::close_tls(peer);
        close(listen_fd);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto silent=opal::connect_tls(client_ctx,"127.0.0.1",silent_port);
    assert(silent.ssl);
    started=std::chrono::steady_clock::now();
    line.clear();
    assert(!opal::tls_read_line_timeout(silent.ssl,line,200));
    elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count();
    assert(elapsed>=150);
    assert(elapsed<1000);
    opal::close_tls(silent);
    silent_server.join();

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
