#include <opal/tunnel.hpp>
#include <algorithm>
#include <chrono>
#include <thread>

namespace opal {
void tunnel_host_supervise(std::atomic<bool>&run,int interval_ms){
    interval_ms=std::max(10,interval_ms);
    while(run.load()){
        int remaining=interval_ms;
        while(run.load()&&remaining>0){
            int slice=std::min(remaining,100);
            std::this_thread::sleep_for(std::chrono::milliseconds(slice));
            remaining-=slice;
        }
        if(!run.load())break;
        if(!tunnel_host_healthy())tunnel_host_ensure_running();
    }
}
}
