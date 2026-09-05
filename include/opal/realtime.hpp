#pragma once

#include <climits>
#include <cstdint>

#if defined(__linux__)
#include <sys/prctl.h>
#endif

namespace opal {

inline std::uint64_t timer_slack_ns(){
#if defined(__linux__)
    const int value=prctl(PR_GET_TIMERSLACK,0UL,0UL,0UL,0UL);
    return value>0?static_cast<std::uint64_t>(value):0;
#else
    return 0;
#endif
}

inline bool set_low_latency_timer_slack(std::uint64_t nanoseconds=1000){
#if defined(__linux__)
    if(nanoseconds==0||nanoseconds>static_cast<std::uint64_t>(ULONG_MAX))return false;
    return prctl(PR_SET_TIMERSLACK,static_cast<unsigned long>(nanoseconds),0UL,0UL,0UL)==0;
#else
    (void)nanoseconds;
    return false;
#endif
}

}
