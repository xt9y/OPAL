#include <opal/tunnel.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/stat.h>

namespace fs=std::filesystem;

static std::string read_all(const fs::path&path){
    std::ifstream in(path);
    return std::string((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>());
}

int main(){
    auto root=fs::temp_directory_path()/"opal-zrok-stale-share-recovery";
    fs::remove_all(root);
    fs::create_directories(root/"bin");
    fs::create_directories(root/"shares");
    fs::create_directories(root/"opal");

    setenv("OPAL_HOME",(root/"opal").c_str(),1);
    setenv("ZROK_TEST_LOG",(root/"zrok.log").c_str(),1);
    setenv("ZROK_TEST_SHARE_DIR",(root/"shares").c_str(),1);
    std::string old_path=std::getenv("PATH")?std::getenv("PATH"):"";
    std::string path=(root/"bin").string()+":"+old_path;
    setenv("PATH",path.c_str(),1);

    auto script=root/"bin/zrok2";
    std::ofstream z(script);
    z << R"SH(#!/bin/sh
printf '%s\n' "$*" >> "$ZROK_TEST_LOG"
case "$1" in
  status)
    echo 'Environment:'
    echo 'EnvZId <<SET>>'
    exit 0
    ;;
  create)
    [ "$2" = share ] || exit 61
    [ "$3" = --share-token ] || exit 62
    [ -n "$4" ] || exit 63
    touch "$ZROK_TEST_SHARE_DIR/$4"
    exit 0
    ;;
  share)
    [ "$2" = private ] || exit 71
    token=''
    prev=''
    for arg in "$@"; do
      if [ "$prev" = --share-token ]; then token="$arg"; break; fi
      prev="$arg"
    done
    [ -n "$token" ] || exit 72
    [ -f "$ZROK_TEST_SHARE_DIR/$token" ] || exit 74
    sleep 5
    ;;
  delete)
    [ "$2" = share ] || exit 81
    rm -f "$ZROK_TEST_SHARE_DIR/$3"
    exit 0
    ;;
esac
exit 0
)SH";
    z.close();
    chmod(script.c_str(),0755);

    std::ofstream host(root/"opal/host.ini");
    host << "[tunnel]\n"
            "control_token=opal-ctl-preserved\n"
            "video_token=opal-vid-preserved\n"
            "mode=zrok2-private\n";
    host.close();

    // The local config remembers the connection code, but the corresponding
    // persistent zrok shares have disappeared server-side. Restart must repair
    // those exact names instead of forcing every client to save a new code.
    if(opal::tunnel_host_start()!=0){
        std::cerr << "stale persistent share was not recovered\n";
        return 1;
    }

    auto log=read_all(root/"zrok.log");
    if(log.find("create share --share-token opal-ctl-preserved --backend-mode tcpTunnel")==std::string::npos) return 2;
    if(log.find("create share --share-token opal-vid-preserved --backend-mode tcpTunnel")==std::string::npos) return 3;

    opal::tunnel_clean_local();
    std::cout << "tunnel recovery tests passed\n";
    return 0;
}
