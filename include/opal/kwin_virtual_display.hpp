#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace opal {

class KwinVirtualDisplay {
public:
    KwinVirtualDisplay();
    KwinVirtualDisplay(const KwinVirtualDisplay&) = delete;
    KwinVirtualDisplay& operator=(const KwinVirtualDisplay&) = delete;
    ~KwinVirtualDisplay();

    bool connect();
    bool stream_existing_output(std::uint32_t& pipewire_node);
    bool create_virtual_output(const std::string& name, int width, int height, float scale,
                               std::uint32_t& pipewire_node);
    void close();

    bool has_output() const;
    std::string last_error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
