#include <opal/tunnel.hpp>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/stat.h>

namespace fs = std::filesystem;

static std::string original_path;

static fs::path make_fake_zrok(const char *name,const char *mode) {
    auto root=fs::temp_directory_path()/name;
    fs::remove_all(root);
    fs::create_directories(root/"bin");
    setenv("OPAL_HOME",(root/"opal").c_str(),1);
    setenv("ZROK_TEST_LOG",(root/"zrok.log").c_str(),1);
    setenv("ZROK_TEST_MARKER",(root/"enabled").c_str(),1);
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
    if [ "$ZROK_TEST_MODE" = enabled ] || [ -f "$ZROK_TEST_MARKER" ]; then
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
    sleep 2
    exit 0
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
        assert(opal::tunnel_host_start()==0);
        log=read_all(root/"zrok.log");
        assert(log.find("share private --headless --share-token opal-ctl-")!=std::string::npos);
        assert(log.find("share private --headless --share-token opal-vid-")!=std::string::npos);
    }

    std::cout << "tunnel tests passed\n";
}
