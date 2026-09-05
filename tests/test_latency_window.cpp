#include <opal/latency_window.hpp>
#include <cassert>
#include <cmath>

static bool near(double a,double b,double epsilon=0.001){return std::fabs(a-b)<=epsilon;}

int main(){
    opal::LatencyWindow<8> window;
    auto empty=window.snapshot();assert(empty.samples==0&&empty.p50==0.0&&empty.p95==0.0&&empty.p99==0.0);
    for(int i=1;i<=8;++i)window.push(static_cast<double>(i));
    auto full=window.snapshot();assert(full.samples==8);assert(near(full.p50,4.5));assert(near(full.p95,7.65));assert(near(full.p99,7.93));
    window.push(100.0);auto wrapped=window.snapshot();assert(wrapped.samples==8);assert(wrapped.p99>93.0&&wrapped.p95>67.0);
    window.push(-1.0);assert(window.snapshot().samples==8);
    window.clear();assert(window.size()==0&&window.snapshot().samples==0);
    return 0;
}
