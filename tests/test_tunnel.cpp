#include <opal/tunnel.hpp>
#include <opal/config.hpp>
#include <arpa/inet.h>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <signal.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace fs=std::filesystem;
static std::string original_path;

static fs::path make_fake_zrok(const char *name,const char *mode){
    auto root=fs::temp_directory_path()/name;fs::remove_all(root);fs::create_directories(root/"bin");fs::create_directories(root/"state");
    setenv("OPAL_HOME",(root/"opal").c_str(),1);setenv("ZROK_TEST_LOG",(root/"zrok.log").c_str(),1);setenv("ZROK_TEST_MARKER",(root/"enabled").c_str(),1);setenv("ZROK_TEST_PIDS",(root/"pids").c_str(),1);setenv("ZROK_TEST_STATE_DIR",(root/"state").c_str(),1);setenv("ZROK_TEST_MODE",mode,1);
    std::string path=(root/"bin").string()+":"+original_path;setenv("PATH",path.c_str(),1);
    auto script=root/"bin/zrok2";std::ofstream out(script);
    out<<R"SH(#!/bin/sh
printf '%s\n' "$*" >> "$ZROK_TEST_LOG"
case "$1" in
  status)
    echo 'Config:'
    if [ "$ZROK_TEST_MODE" != disabled ] || [ -f "$ZROK_TEST_MARKER" ]; then echo 'Environment:'; echo 'EnvZId <<SET>>'; fi
    exit 0 ;;
  enable)
    touch "$ZROK_TEST_MARKER"; exit 0 ;;
  create)
    [ "$2" = share ] || exit 61
    [ "$3" = --share-token ] || exit 62
    [ -n "$4" ] || exit 63
    [ "$5" = --backend-mode ] || exit 64
    [ "$6" = tcpTunnel ] || exit 65
    touch "$ZROK_TEST_STATE_DIR/share_$4"; exit 0 ;;
  list)
    [ "$2" = shares ] || exit 66
    for f in "$ZROK_TEST_STATE_DIR"/share_*; do [ -e "$f" ] || continue; basename "$f" | sed 's/^share_//'; done
    exit 0 ;;
  delete)
    [ "$2" = share ] || exit 67
    rm -f "$ZROK_TEST_STATE_DIR/share_$3"; exit 0 ;;
  share)
    [ "$2" = private ] || exit 71
    printf '%s\n' "$*" | grep -q '127.0.0.1:47990' || exit 72
    printf '%s\n' "$*" | grep -q '47991' && exit 73
    echo $$ >> "$ZROK_TEST_PIDS"
    sleep 20 ;;
  access)
    [ "$2" = private ] || exit 81
    token="$3"; port=''
    for arg in "$@"; do case "$arg" in 127.0.0.1:*) port="${arg##*:}" ;; esac; done
    [ "$port" = 47990 ] || exit 82
    printf '%s\n' "$*" | grep -q '47991' && exit 83
    if [ "$ZROK_TEST_MODE" = access-first-fails ]; then
      marker="$ZROK_TEST_STATE_DIR/access_$token"
      if [ ! -f "$marker" ]; then touch "$marker"; echo 'accessNotFound' >&2; exit 44; fi
    fi
    echo $$ >> "$ZROK_TEST_PIDS"
    exec python3 - "$port" <<'PY'
import socket,sys,time
port=int(sys.argv[1]);s=socket.socket();s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1);s.bind(('127.0.0.1',port));s.listen(8);s.settimeout(.2);end=time.time()+20
while time.time()<end:
    try:
        c,_=s.accept();c.close()
    except socket.timeout: pass
PY
    ;;
esac
exit 0
)SH";
    out.close();chmod(script.c_str(),0755);return root;
}

static std::string read_all(const fs::path&p){std::ifstream in(p);return std::string((std::istreambuf_iterator<char>(in)),{});}
static size_t count_text(const std::string&text,const std::string&needle){size_t n=0,pos=0;while((pos=text.find(needle,pos))!=std::string::npos){++n;pos+=needle.size();}return n;}
static bool can_connect(uint16_t port){int fd=socket(AF_INET,SOCK_STREAM,0);if(fd<0)return false;sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(port);inet_pton(AF_INET,"127.0.0.1",&a.sin_addr);bool ok=connect(fd,reinterpret_cast<sockaddr*>(&a),sizeof(a))==0;close(fd);return ok;}
static std::vector<pid_t> read_pids(const fs::path&root){std::ifstream in(root/"pids");std::vector<pid_t> pids;pid_t p=0;while(in>>p)pids.push_back(p);return pids;}
static bool alive(pid_t pid){return pid>0&&kill(pid,0)==0;}
static void stop_fake(const fs::path&root){for(auto pid:read_pids(root)){if(alive(pid))kill(pid,SIGTERM);waitpid(pid,nullptr,WNOHANG);}}

int main(){
    original_path=std::getenv("PATH")?std::getenv("PATH"):"";

    {
        auto root=make_fake_zrok("opal-zrok-enable-test","disabled");
        std::istringstream input("TEST-ENABLE-TOKEN\n");auto *old=std::cin.rdbuf(input.rdbuf());std::string code;assert(opal::tunnel_host_setup(code)==0);std::cin.rdbuf(old);
        assert(code.rfind("opal:opal-ctl-",0)==0);assert(code.find(',')==std::string::npos);
        auto log=read_all(root/"zrok.log");assert(log.find("enable TEST-ENABLE-TOKEN")!=std::string::npos);
        assert(count_text(log,"create share --share-token opal-ctl-")==1);assert(log.find("opal-vid-")==std::string::npos);
    }
    {
        auto root=make_fake_zrok("opal-zrok-control-only-setup","enabled");std::string code;assert(opal::tunnel_host_setup(code)==0);
        assert(code.rfind("opal:opal-ctl-",0)==0&&code.find(',')==std::string::npos);
        auto log=read_all(root/"zrok.log");assert(count_text(log,"create share --share-token opal-ctl-")==1);assert(log.find("opal-vid-")==std::string::npos);
        assert(opal::tunnel_host_start()==0);std::this_thread::sleep_for(std::chrono::milliseconds(100));
        log=read_all(root/"zrok.log");assert(log.find("share private --headless --share-token opal-ctl-")!=std::string::npos);assert(log.find("47991")==std::string::npos);
        assert(opal::tunnel_host_healthy());stop_fake(root);
    }
    {
        auto root=make_fake_zrok("opal-zrok-legacy-retire","enabled");
        auto paths=opal::Paths::load();assert(opal::ensure_layout(paths));opal::Ini host;host.set("tunnel","control_token","control-token");host.set("tunnel","video_token","legacy-video-token");assert(host.save(paths.host));
        {std::ofstream(root/"state/share_control-token");std::ofstream(root/"state/share_legacy-video-token");}
        std::string code;assert(opal::tunnel_host_setup(code)==0);assert(code=="opal:control-token");
        opal::Ini saved;assert(saved.load(paths.host));assert(saved.get("tunnel","video_token").empty());
        auto log=read_all(root/"zrok.log");assert(log.find("delete share legacy-video-token")!=std::string::npos);assert(!fs::exists(root/"state/share_legacy-video-token"));
    }
    {
        std::string control,legacy;
        assert(opal::tunnel_connection_code("opal:control",&control,&legacy)&&control=="control"&&legacy.empty());
        assert(opal::tunnel_connection_code("opal:control,old-video",&control,&legacy)&&control=="control"&&legacy=="old-video");
        assert(!opal::tunnel_connection_code("opal:",nullptr,nullptr));
        assert(!opal::tunnel_connection_code("opal:a,b,c",nullptr,nullptr));
    }
    {
        auto root=make_fake_zrok("opal-zrok-access-control-only","enabled");
        assert(opal::tunnel_access("control-a",5000));assert(can_connect(47990));
        auto first=read_pids(root);assert(first.size()==1&&alive(first.front()));
        assert(opal::tunnel_access("control-b",5000));assert(can_connect(47990));
        for(auto pid:first)assert(!alive(pid));
        auto log=read_all(root/"zrok.log");assert(log.find("access private control-a")!=std::string::npos);assert(log.find("access private control-b")!=std::string::npos);assert(log.find("47991")==std::string::npos);
        stop_fake(root);
    }
    {
        auto root=make_fake_zrok("opal-zrok-access-propagation","access-first-fails");auto started=std::chrono::steady_clock::now();assert(opal::tunnel_access("retry-control",5000));
        auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count();assert(elapsed>=900);
        auto log=read_all(root/"zrok.log");assert(count_text(log,"access private retry-control")==2);assert(log.find("47991")==std::string::npos);stop_fake(root);
    }

    std::cout<<"tunnel tests passed\n";
}
