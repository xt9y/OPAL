#include <opal/tunnel.hpp>
#include <atomic>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <signal.h>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs=std::filesystem;

static std::string read_all(const fs::path&path){
    std::ifstream in(path);
    return std::string((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>());
}

static std::vector<pid_t> read_pids(const fs::path&path){
    std::ifstream in(path);
    std::vector<pid_t> out;
    pid_t pid=0;
    while(in>>pid)out.push_back(pid);
    return out;
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
    setenv("ZROK_TEST_PIDS",(root/"pids").c_str(),1);
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
    echo $$ >> "$ZROK_TEST_PIDS"
    sleep 30
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

    assert(opal::tunnel_host_start()==0);
    assert(opal::tunnel_host_healthy());

    auto log=read_all(root/"zrok.log");
    assert(log.find("create share --share-token opal-ctl-preserved --backend-mode tcpTunnel")!=std::string::npos);
    assert(log.find("create share --share-token opal-vid-preserved --backend-mode tcpTunnel")!=std::string::npos);

    // Continuous supervision must repair a child that dies after startup,
    // without requiring an explicit restart and without changing share tokens.
    std::atomic<bool> supervise{true};
    std::thread supervisor([&]{opal::tunnel_host_supervise(supervise,50);});
    auto pids=read_pids(root/"pids");
    assert(pids.size()>=2);
    const pid_t killed_pid=pids.front();
    kill(killed_pid,SIGTERM);
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(4);
    bool repaired=false;
    while(std::chrono::steady_clock::now()<deadline){
        // Do not mistake the short SIGTERM delivery window for recovery. The
        // old test could observe the killed process as still healthy, stop the
        // supervisor immediately, and then fail once that process finally
        // exited. Recovery is complete only after the recorded host pair has
        // actually been replaced and the replacement pair is healthy.
        auto current=read_pids(root/"opal/tunnel-host.pids");
        bool old_replaced=current.size()==2;
        for(pid_t pid:current) if(pid==killed_pid) old_replaced=false;
        if(old_replaced&&opal::tunnel_host_healthy()){repaired=true;break;}
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    supervise.store(false);
    supervisor.join();
    assert(repaired);
    assert(opal::tunnel_host_healthy());

    auto config=read_all(root/"opal/host.ini");
    assert(config.find("control_token=opal-ctl-preserved")!=std::string::npos);
    assert(config.find("video_token=opal-vid-preserved")!=std::string::npos);
    log=read_all(root/"zrok.log");
    assert(log.find("share private --headless --share-token opal-ctl-preserved")!=std::string::npos);
    assert(log.find("share private --headless --share-token opal-vid-preserved")!=std::string::npos);

    opal::tunnel_clean_local();
    std::cout << "tunnel recovery tests passed\n";
    return 0;
}
