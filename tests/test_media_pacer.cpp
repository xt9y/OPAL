#include <opal/media_pacer.hpp>

#include <cassert>
#include <cstdint>

int main(){
    assert(opal::pacer_sleep_slice_us(0)==0);
    assert(opal::pacer_sleep_slice_us(1)==1);
    assert(opal::pacer_sleep_slice_us(24)==24);
    assert(opal::pacer_sleep_slice_us(250)==250);
    assert(opal::pacer_sleep_slice_us(1000)==250);

    opal::MediaTokenBucket pacer;
    pacer.reset(1200.0,1000000);
    assert(pacer.reserve_or_delay_us(1200,1200.0,8000,1000000)==0);
    const auto wait=pacer.reserve_or_delay_us(1200,1200.0,8000,1000000);
    assert(wait>=1199&&wait<=1201);
    assert(pacer.reserve_or_delay_us(1200,1200.0,8000,1001200)==0);

    pacer.reset(2400.0,2000000);
    assert(pacer.reserve_or_delay_us(1200,2400.0,8000,2000000)==0);
    assert(pacer.reserve_or_delay_us(1200,2400.0,8000,2000000)==0);
    assert(pacer.reserve_or_delay_us(1,2400.0,8000,2000000)>=1);
    pacer.top_up(2400.0,2000000);
    assert(pacer.reserve_or_delay_us(2400,2400.0,8000,2000000)==0);
    return 0;
}
