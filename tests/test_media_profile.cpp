#include <opal/media_profile.hpp>
#include <cassert>

int main() {
    auto d=opal::default_stream_options();
    assert(d.max_width==1920);
    assert(d.max_height==1080);
    assert(d.fps==60);

    int w=-1,h=-1;
    assert(opal::stream_mode_limit("max",w,h)&&w==0&&h==0);
    assert(opal::stream_mode_limit("1080p",w,h)&&w==1920&&h==1080);
    assert(opal::stream_mode_limit("1440p",w,h)&&w==2560&&h==1440);
    assert(opal::stream_mode_limit("4k",w,h)&&w==3840&&h==2160);
    assert(!opal::stream_mode_limit("invalid",w,h));

    assert(opal::automatic_bitrate_kbps(1920,1080,60)==30000);
    assert(opal::capture_stale_budget_us(60)>=32000&&opal::capture_stale_budget_us(60)<=34000);
    assert(opal::capture_stale_budget_us(120)>=16000&&opal::capture_stale_budget_us(120)<=17000);
    assert(opal::capture_stale_budget_us(240)>=8000&&opal::capture_stale_budget_us(240)<=9000);
    assert(opal::capture_stale_budget_us(15)>=133000&&opal::capture_stale_budget_us(15)<=134000);
    assert(opal::normal_gop_frames(60)==15);
    assert(opal::normal_gop_frames(120)==30);
    assert(opal::normal_gop_frames(15)==4);
    assert(opal::normal_gop_frames(240)==60);

    // IDRs stay tightly paced so a large intra frame cannot monopolize the
    // shared UDP socket. Ordinary frames need a frame-sized token budget so
    // normal 60 FPS traffic does not consume the entire frame deadline and
    // spuriously trigger send-failure capture restarts.
    constexpr std::uint64_t two_datagrams=2*1200;
    assert(opal::sender_burst_budget_bytes(30000,60,true)==two_datagrams);
    assert(opal::sender_burst_budget_bytes(100000,15,true)==two_datagrams);
    assert(opal::sender_burst_budget_bytes(30000,60,false)==128ULL*1024ULL);
    assert(opal::sender_burst_budget_bytes(20000,15,false)==333332ULL);
    assert(opal::sender_burst_budget_bytes(100000,15,false)==512ULL*1024ULL);
    assert(opal::sender_burst_budget_bytes(1,1000,false)==128ULL*1024ULL);

    assert(opal::sender_pacing_rate_kbps(30000,false)==36000);
    assert(opal::sender_pacing_rate_kbps(30000,true)==120000);
    assert(opal::sender_pacing_rate_kbps(100000,false)==120000);
    assert(opal::sender_pacing_rate_kbps(100000,true)==400000);
    assert(opal::sender_pacing_rate_kbps(1,false)==1200);
    assert(opal::sender_pacing_rate_kbps(1,true)==4000);
    return 0;
}
