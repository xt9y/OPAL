#include <opal/multimonitor.hpp>

#include <cassert>
#include <cstdint>
#include <vector>

using namespace opal;

int main()
{
    {
        std::vector<MonitorGeometry> monitors = {
            {2560, 1440, 0, 0, 2560, 1440, true},
            {1920, 1080, 2560, 180, 1920, 1080, true},
        };
        const auto layout = build_composite_layout(monitors, 7680, 4320);
        assert(layout.valid());
        assert(layout.monitors.size() == 2);
        assert(layout.tile_width == 2560);
        assert(layout.tile_height == 1440);
        assert(layout.canvas_width == 5120);
        assert(layout.canvas_height == 1440);
        assert(layout.monitors[0].tile_x == 0);
        assert(layout.monitors[1].tile_x == 2560);
        assert(layout.monitors[0].tile_width == layout.monitors[1].tile_width);
        assert(layout.monitors[0].tile_height == layout.monitors[1].tile_height);
    }

    {
        std::vector<MonitorGeometry> monitors = {
            {3840, 2160, 0, 0, 3840, 2160, true},
            {1920, 1080, 3840, 0, 1920, 1080, true},
        };
        const auto layout = build_composite_layout(monitors, 3840, 2160);
        assert(layout.valid());
        assert(layout.tile_width == 1920);
        assert(layout.tile_height == 1080);
        assert(layout.canvas_width == 3840);
        assert(layout.canvas_height == 1080);
    }

    {
        std::vector<MonitorGeometry> monitors = {
            {1921, 1081, 0, 0, 1921, 1081, true},
            {1367, 769, 1921, 0, 1367, 769, true},
        };
        const auto layout = build_composite_layout(monitors, 3839, 2159);
        assert(layout.valid());
        assert((layout.tile_width % 2) == 0);
        assert((layout.tile_height % 2) == 0);
        assert((layout.canvas_width % 2) == 0);
        assert((layout.canvas_height % 2) == 0);
        assert(layout.canvas_width <= 3839);
        assert(layout.canvas_height <= 2159);
    }

    {
        std::vector<MonitorGeometry> monitors = {
            {1920, 1080, -1920, 200, 1920, 1080, true},
            {2560, 1440, 0, 0, 2560, 1440, true},
            {1280, 1024, 2560, 100, 1280, 1024, true},
        };
        const auto layout = build_composite_layout(monitors, 16384, 16384);
        assert(layout.valid());
        assert(layout.monitors.size() == 3);
        assert(layout.canvas_width == layout.tile_width * 3);

        const auto left = map_composite_pointer(layout, 0, 32768);
        const auto middle = map_composite_pointer(layout, 32768, 32768);
        const auto right = map_composite_pointer(layout, 65535, 32768);
        assert(left.first < middle.first);
        assert(middle.first < right.first);
        assert(left.first >= 0 && left.first <= 65535);
        assert(right.first >= 0 && right.first <= 65535);
        assert(left.second >= 0 && left.second <= 65535);
    }

    {
        std::vector<MonitorGeometry> monitors = {
            {1600, 900, 0, 0, 0, 0, false},
            {1280, 1024, 0, 0, 0, 0, false},
        };
        const auto layout = build_composite_layout(monitors, 4096, 2160);
        assert(layout.valid());
        assert(layout.monitors.size() == 2);
        assert(layout.monitors[0].logical_x == 0);
        assert(layout.monitors[1].logical_x == 1600);
        assert(layout.desktop_min_x == 0);
        assert(layout.desktop_width == 2880);
    }

    {
        const std::vector<MonitorGeometry> monitors;
        const auto layout = build_composite_layout(monitors, 1920, 1080);
        assert(!layout.valid());
        const auto passthrough = map_composite_pointer(layout, 100, 100);
        assert(passthrough.first == 100);
        assert(passthrough.second == 100);
    }

    return 0;
}
