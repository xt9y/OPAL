#include <opal/config.hpp>
#include <opal/crypto.hpp>
#include <opal/media.hpp>
#include <opal/wake.hpp>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <iostream>

int main() {
    char t[] = "/tmp/opal-test-XXXXXX";
    char *d = mkdtemp(t);
    assert(d);
    setenv("OPAL_HOME", d, 1);
    auto paths = opal::Paths::load();
    assert(paths.root == std::filesystem::path(d));
    assert(opal::ensure_layout(paths));

    opal::Ini ini;
    ini.set("host", "name", "desktop");
    ini.set("video", "fps", "60");
    assert(ini.save(paths.config));
    opal::Ini read;
    assert(read.load(paths.config));
    assert(read.get("host", "name") == "desktop");
    assert(read.get_int("video", "fps", 0) == 60);

    auto pair = opal::pairing_code();
    assert(pair.size() == 9 && pair[4] == '-');
    auto nonce = opal::random_hex(32);
    auto proof = opal::hmac_sha256_hex("secret", nonce);
    assert(opal::secure_equal(proof, opal::hmac_sha256_hex("secret", nonce)));
    assert(!opal::secure_equal(proof, opal::hmac_sha256_hex("wrong", nonce)));

    auto cmd = opal::capture_command(true, 60, 20000, true);
    assert(cmd.find("gpu-screen-recorder") != std::string::npos);
    assert(cmd.find(" -f 60 ") != std::string::npos);
    assert(cmd.find(" -k h264 ") != std::string::npos);
    assert(cmd.find(" -v h264 ") == std::string::npos);
    assert(cmd.find(" -bm cbr ") != std::string::npos);
    assert(cmd.find(" -q 20000") != std::string::npos);
    assert(cmd.find(" -a default_output") != std::string::npos);
    assert(cmd.find(" -c flv ") != std::string::npos);
    assert(cmd.find(" -o -") != std::string::npos);
    assert(cmd.find("WAYLAND_DISPLAY:-screen") == std::string::npos);
    assert(cmd.find("portalwayland") == std::string::npos);

    auto fallback = opal::capture_command(false, 60, 12000, false);
    assert(fallback.find("ffmpeg") != std::string::npos);
    assert(fallback.find("x11grab") != std::string::npos);

    auto packet = opal::wol_packet("00:11:22:33:44:55");
    assert(packet.size() == 102);
    for (int i=0;i<6;i++) assert(packet[i] == 0xff);
    for (int i=0;i<16;i++) {
        assert(packet[6+i*6+0] == 0x00);
        assert(packet[6+i*6+5] == 0x55);
    }

    std::cout << "core tests passed\n";
}
