#include <opal/tunnel.hpp>
#include <arpa/inet.h>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <fcntl.h>
#include <signal.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

static std::string original_path;

static fs::path make_fake_zrok(const char *name,const char *mode) {
    auto root=fs::temp_directory_path()/name;
    fs::remove_all(root);
    fs::create_directories(root/"bin");
    fs::create_directories(root/"state");
    setenv("OPAL_HOME",(root/"opal").c_str(),1);
    setenv("ZROK_TEST_LOG",(root/"zrok.log").c_str(),1);
    setenv("ZROK_TEST_MARKER",(root/"enabled").c_str(),1);
    setenv("ZROK_TEST_PIDS",(root/"pids").c_str(),1);
    setenv("ZROK_TEST_STATE_DIR",(root/"state").c_str(),1);
    setenv("ZROK_TEST_MODE",mode,1);
    std::string path=(root/"bin").string()+":"+original_path;
    setenv("PATH",path.c_str(),1);

    auto script=root/"bin/zrok2";
    std::ofstream out(script);
    out << R"SH(#!/bin/sh
printf '%s\n' "$*" >> "$ZROK_TEST_LOG"
case "$1" in
  status)
    echo 'Config:'
    if [ "$ZROK_TEST_MODE" = enabled ] || [ "$ZROK_TEST_MODE" = access-delayed ] || [ "$ZROK_TEST_MODE" = access-slow ] || [ "$ZROK_TEST_MODE" = access-first-fails ] || [ -f "$ZROK_TEST_MARKER" ]; then
      echo 'Environment:'
      echo 'EnvZId <<SET>>'
    else
      echo 'To create a local environment use the zrok2 enable command.' >&2
    fi
    exit 0
    ;;
  enable)
    touch "$ZROK_TEST_MARKER"
    exit 0
    ;;
  create)
    if [ "$ZROK_TEST_MODE" = disabled ]; then
      exit 0
    fi
    [ "$2" = share ] || exit 61
    [ "$3" = --share-token ] || exit 62
    [ -n "$4" ] || exit 63
    [ "$5" = --backend-mode ] || exit 64
    [ "$6" = tcpTunnel ] || exit 65
    exit 0
    ;;
  share)
    [ "$2" = private ] || exit 71
    printf '%s\n' "$*" | grep -q -- '--share-token' || exit 72
    if printf '%s\n' "$*" | grep -q -- '--backend-mode'; then
      exit 73
    fi
    echo 'ZROK_DEBUG_NOISE_STDOUT'
    echo 'ZROK_DEBUG_NOISE_STDERR' >&2
    sleep 2
    exit 0
    ;;
  access)
    [ "$ZROK_TEST_MODE" = access-delayed ] || [ "$ZROK_TEST_MODE" = access-slow ] || [ "$ZROK_TEST_MODE" = access-first-fails ] || exit 0
    echo 'ZROK_DEBUG_NOISE_STDOUT'
    echo 'ZROK_DEBUG_NOISE_STDERR' >&2
    case "$*" in
      *127.0.0.1:47990*) port=47990; delay=1 ;;
      *127.0.0.1:47991*) port=47991; delay=2 ;;
      *) exit 81 ;;
    esac
    if [ "$ZROK_TEST_MODE" = access-slow ]; then
      if [ "$port" = 47991 ]; then delay=5; fi
    fi
    if [ "$ZROK_TEST_MODE" = access-first-fails ]; then
      marker="$ZROK_TEST_STATE_DIR/$port"
      if [ ! -f "$marker" ]; then
        touch "$marker"
        echo 'accessNotFound: share is still propagating' >&2
        exit 44
      fi
      delay=0
    fi
    echo $$ >> "$ZROK_TEST_PIDS"
    sleep "$delay"
    exec python3 - "$port" <<'PY'
import socket
import sys
import time
port = int(sys.argv[1])
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('127.0.0.1', port))
s.listen(8)
s.settimeout(0.25)
end = time.time() + 12
while time.time() < end:
    try:
        conn, _ = s.accept()
        conn.close()
    except socket.timeout:
        pass
PY
    ;;
esac
exit 0
)SH";
    out.close();
    chmod(script.c_str(),0755);
    return root;
}

static std::string read_all(const fs::path &path) {
    std::ifstream in(path);
    return std::string((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>());
}

static size_t count_text(const std::string &haystack,const std::string &needle) {
    size_t count=0,pos=0;
    while((pos=haystack.find(needle,pos))!=std::string::npos){++count;pos+=needle.size();}
    return count;
}

static std::string capture_fds(const fs::path &path,const std::function<void()> &fn) {
    std::cout.flush();
    std::cerr.flush();
    fflush(stdout);
    fflush(stderr);
    int saved_out=dup(STDOUT_FILENO);
    int saved_err=dup(STDERR_FILENO);
    int fd=open(path.c_str(),O_CREAT|O_TRUNC|O_WRONLY,0600);
    assert(saved_out>=0&&saved_err>=0&&fd>=0);
    assert(dup2(fd,STDOUT_FILENO)>=0);
    assert(dup2(fd,STDERR_FILENO)>=0);
    close(fd);
    fn();
    std::cout.flush();
    std::cerr.flush();
    fflush(stdout);
    fflush(stderr);
    assert(dup2(saved_out,STDOUT_FILENO)>=0);
    assert(dup2(saved_err,STDERR_FILENO)>=0);
    close(saved_out);
    close(saved_err);
    return read_all(path);
}

static bool can_connect(uint16_t port) {
    int fd=socket(AF_INET,SOCK_STREAM,0);
    if(fd<0) return false;
    sockaddr_in addr{};
    addr.sin_family=AF_INET;
    addr.sin_port=htons(port);
    inet_pton(AF_INET,"127.0.0.1",&addr.sin_addr);
    bool ok=connect(fd,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))==0;
    close(fd);
    return ok;
}

static std::vector<pid_t> read_pids(const fs::path &root) {
    std::ifstream in(root/"pids");
    std::vector<pid_t> pids;
    pid_t pid=0;
    while(in>>pid) pids.push_back(pid);
    return pids;
}

static bool process_alive(pid_t pid) {
    return pid>0 && kill(pid,0)==0;
}

static void stop_fake_zrok(const fs::path &root) {
    for(auto pid:read_pids(root)) {
        kill(pid,SIGTERM);
        waitpid(pid,nullptr,0);
    }
}

int main() {
    original_path=std::getenv("PATH")?std::getenv("PATH"):"";

    {
        auto root=make_fake_zrok("opal-zrok-disabled-test","disabled");
        std::istringstream input("TEST-ENABLE-TOKEN\n");
        std::ostringstream output;
        auto *old_in=std::cin.rdbuf(input.rdbuf());
        auto *old_out=std::cout.rdbuf(output.rdbuf());
        std::string code;
        int rc=opal::tunnel_host_setup(code);
        std::cin.rdbuf(old_in);
        std::cout.rdbuf(old_out);
        assert(rc==0);
        auto guide=output.str();
        assert(guide.find("https://myzrok.io")!=std::string::npos);
        assert(guide.find("Link zrok Account")!=std::string::npos);
        assert(guide.find("bottom-left")!=std::string::npos);
        assert(guide.find("Get Started")!=std::string::npos);
        assert(guide.find("zrok enable")!=std::string::npos);
        auto log=read_all(root/"zrok.log");
        assert(log.find("status\n")!=std::string::npos);
        assert(log.find("enable TEST-ENABLE-TOKEN\n")!=std::string::npos);
        assert(log.find("status\n",log.find("enable TEST-ENABLE-TOKEN\n"))!=std::string::npos);
    }

    {
        auto root=make_fake_zrok("opal-zrok-create-share-test","enabled");
        std::string code;
        assert(opal::tunnel_host_setup(code)==0);
        assert(code.rfind("opal:opal-ctl-",0)==0);
        auto log=read_all(root/"zrok.log");
        assert(log.find("create share --share-token opal-ctl-")!=std::string::npos);
        assert(log.find("--backend-mode tcpTunnel")!=std::string::npos);
        assert(log.find("create share --share-token opal-vid-")!=std::string::npos);
        int rc=-1;
        auto output=capture_fds(root/"host-output.log",[&]{rc=opal::tunnel_host_start();});
        assert(rc==0);
        assert(output.find("ZROK_DEBUG_NOISE")==std::string::npos);
        log=read_all(root/"zrok.log");
        assert(log.find("share private --headless --share-token opal-ctl-")!=std::string::npos);
        assert(log.find("share private --headless --share-token opal-vid-")!=std::string::npos);
    }

    {
        auto root=make_fake_zrok("opal-zrok-access-readiness-test","access-delayed");
        bool accessed=false;
        auto output=capture_fds(root/"access-output.log",[&]{accessed=opal::tunnel_access("old-control-token","old-video-token");});
        assert(accessed);
        assert(output.find("ZROK_DEBUG_NOISE")==std::string::npos);
        assert(can_connect(47990));
        assert(can_connect(47991));
        auto old_pids=read_pids(root);
        assert(old_pids.size()==2);
        assert(process_alive(old_pids[0]));
        assert(process_alive(old_pids[1]));

        assert(opal::tunnel_access("new-control-token","new-video-token"));
        for(auto pid:old_pids) assert(!process_alive(pid));
        auto log=read_all(root/"zrok.log");
        assert(log.find("access private new-control-token")!=std::string::npos);
        assert(log.find("access private new-video-token")!=std::string::npos);
        stop_fake_zrok(root);
    }

    {
        auto root=make_fake_zrok("opal-zrok-access-backoff-test","access-first-fails");
        bool accessed=false;
        auto started=std::chrono::steady_clock::now();
        auto output=capture_fds(root/"access-backoff-output.log",[&]{accessed=opal::tunnel_access("backoff-control-token","backoff-video-token",5000);});
        auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count();
        assert(accessed);
        assert(output.find("accessNotFound")==std::string::npos);
        assert(elapsed>=900);
        auto log=read_all(root/"zrok.log");
        assert(count_text(log,"access private backoff-control-token")==2);
        assert(count_text(log,"access private backoff-video-token")==2);
        stop_fake_zrok(root);
    }

    {
        auto root=make_fake_zrok("opal-zrok-access-slow-start-test","access-slow");
        bool accessed=false;
        auto started=std::chrono::steady_clock::now();
        auto output=capture_fds(root/"access-slow-output.log",[&]{accessed=opal::tunnel_access("slow-control-token","slow-video-token",10000);});
        auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count();
        assert(accessed);
        assert(output.find("ZROK_DEBUG_NOISE")==std::string::npos);
        assert(elapsed>=4500);
        auto log=read_all(root/"zrok.log");
        assert(count_text(log,"access private slow-control-token")==1);
        assert(count_text(log,"access private slow-video-token")==1);
        stop_fake_zrok(root);
    }

    {
        auto root=make_fake_zrok("opal-zrok-access-propagation-test","access-first-fails");
        bool accessed=false;
        auto output=capture_fds(root/"access-propagation-output.log",[&]{accessed=opal::tunnel_access("retry-control-token","retry-video-token");});
        assert(accessed);
        assert(output.find("accessNotFound")==std::string::npos);
        assert(can_connect(47990));
        assert(can_connect(47991));
        auto log=read_all(root/"zrok.log");
        assert(count_text(log,"access private retry-control-token")>=2);
        assert(count_text(log,"access private retry-video-token")>=2);
        stop_fake_zrok(root);
    }

    std::cout << "tunnel tests passed\n";
}
