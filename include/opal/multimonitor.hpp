#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace opal {

struct MonitorGeometry {
    int source_width = 0;
    int source_height = 0;
    int logical_x = 0;
    int logical_y = 0;
    int logical_width = 0;
    int logical_height = 0;
    bool logical_geometry_valid = false;
};

struct MonitorTile {
    int tile_x = 0;
    int tile_y = 0;
    int tile_width = 0;
    int tile_height = 0;
    int logical_x = 0;
    int logical_y = 0;
    int logical_width = 0;
    int logical_height = 0;
    int source_width = 0;
    int source_height = 0;
};

struct CompositeLayout {
    int tile_width = 0;
    int tile_height = 0;
    int canvas_width = 0;
    int canvas_height = 0;
    int desktop_min_x = 0;
    int desktop_min_y = 0;
    int desktop_width = 0;
    int desktop_height = 0;
    std::vector<MonitorTile> monitors;

    bool valid() const
    {
        return tile_width > 0 && tile_height > 0 && canvas_width > 0 && canvas_height > 0 &&
               desktop_width > 0 && desktop_height > 0 && !monitors.empty();
    }
};

inline CompositeLayout build_composite_layout(std::span<const MonitorGeometry> input,
                                              int max_width,
                                              int max_height)
{
    CompositeLayout layout;
    std::vector<MonitorGeometry> monitors;
    monitors.reserve(input.size());
    for (const auto& monitor : input) {
        if (monitor.source_width <= 0 || monitor.source_height <= 0) continue;
        monitors.push_back(monitor);
    }
    if (monitors.empty()) return layout;

    int base_tile_width = 1;
    int base_tile_height = 1;
    for (const auto& monitor : monitors) {
        base_tile_width = std::max(base_tile_width, monitor.source_width);
        base_tile_height = std::max(base_tile_height, monitor.source_height);
    }

    const std::int64_t monitor_count = static_cast<std::int64_t>(monitors.size());
    const std::int64_t base_canvas_width = static_cast<std::int64_t>(base_tile_width) * monitor_count;
    const std::int64_t base_canvas_height = base_tile_height;

    std::int64_t scale_num = 1;
    std::int64_t scale_den = 1;
    auto tighten_scale = [&](std::int64_t limit, std::int64_t extent) {
        if (limit <= 0 || extent <= limit) return;
        if (limit * scale_den < extent * scale_num) {
            scale_num = limit;
            scale_den = extent;
        }
    };
    tighten_scale(max_width, base_canvas_width);
    tighten_scale(max_height, base_canvas_height);

    layout.tile_width = std::max(1, static_cast<int>((static_cast<std::int64_t>(base_tile_width) * scale_num) / scale_den));
    layout.tile_height = std::max(1, static_cast<int>((static_cast<std::int64_t>(base_tile_height) * scale_num) / scale_den));
    if (max_width > 0) layout.tile_width = std::min(layout.tile_width, std::max(1, max_width / static_cast<int>(monitors.size())));
    if (max_height > 0) layout.tile_height = std::min(layout.tile_height, max_height);

    // H.264/NV12 paths are most reliable with even frame dimensions. Keep every tile
    // identical by rounding the shared tile size, not individual monitors.
    if (layout.tile_width > 1) layout.tile_width &= ~1;
    if (layout.tile_height > 1) layout.tile_height &= ~1;
    layout.tile_width = std::max(1, layout.tile_width);
    layout.tile_height = std::max(1, layout.tile_height);

    layout.canvas_width = layout.tile_width * static_cast<int>(monitors.size());
    layout.canvas_height = layout.tile_height;

    int fallback_x = 0;
    int min_x = std::numeric_limits<int>::max();
    int min_y = std::numeric_limits<int>::max();
    int max_x = std::numeric_limits<int>::min();
    int max_y = std::numeric_limits<int>::min();
    layout.monitors.reserve(monitors.size());

    for (std::size_t i = 0; i < monitors.size(); ++i) {
        const auto& monitor = monitors[i];
        MonitorTile tile;
        tile.tile_x = static_cast<int>(i) * layout.tile_width;
        tile.tile_y = 0;
        tile.tile_width = layout.tile_width;
        tile.tile_height = layout.tile_height;
        tile.source_width = monitor.source_width;
        tile.source_height = monitor.source_height;
        if (monitor.logical_geometry_valid && monitor.logical_width > 0 && monitor.logical_height > 0) {
            tile.logical_x = monitor.logical_x;
            tile.logical_y = monitor.logical_y;
            tile.logical_width = monitor.logical_width;
            tile.logical_height = monitor.logical_height;
        } else {
            tile.logical_x = fallback_x;
            tile.logical_y = 0;
            tile.logical_width = monitor.source_width;
            tile.logical_height = monitor.source_height;
        }
        fallback_x = tile.logical_x + tile.logical_width;
        min_x = std::min(min_x, tile.logical_x);
        min_y = std::min(min_y, tile.logical_y);
        max_x = std::max(max_x, tile.logical_x + tile.logical_width);
        max_y = std::max(max_y, tile.logical_y + tile.logical_height);
        layout.monitors.push_back(tile);
    }

    layout.desktop_min_x = min_x;
    layout.desktop_min_y = min_y;
    layout.desktop_width = std::max(1, max_x - min_x);
    layout.desktop_height = std::max(1, max_y - min_y);
    return layout;
}

inline std::pair<int, int> map_composite_pointer(const CompositeLayout& layout,
                                                 int x65535,
                                                 int y65535)
{
    if (!layout.valid()) return {x65535, y65535};
    x65535 = std::clamp(x65535, 0, 65535);
    y65535 = std::clamp(y65535, 0, 65535);

    const auto scale_to_extent = [](int value, int extent) {
        if (extent <= 1) return 0;
        return static_cast<int>((static_cast<std::int64_t>(value) * (extent - 1) + 32767) / 65535);
    };
    const int canvas_x = scale_to_extent(x65535, layout.canvas_width);
    const int canvas_y = scale_to_extent(y65535, layout.canvas_height);
    const int index = std::clamp(canvas_x / std::max(1, layout.tile_width), 0,
                                 static_cast<int>(layout.monitors.size()) - 1);
    const auto& tile = layout.monitors[static_cast<std::size_t>(index)];
    const int local_x = std::clamp(canvas_x - tile.tile_x, 0, std::max(0, tile.tile_width - 1));
    const int local_y = std::clamp(canvas_y - tile.tile_y, 0, std::max(0, tile.tile_height - 1));

    const auto map_local = [](int value, int from_extent, int to_extent) {
        if (from_extent <= 1 || to_extent <= 1) return 0;
        return static_cast<int>((static_cast<std::int64_t>(value) * (to_extent - 1) + (from_extent - 1) / 2) /
                                (from_extent - 1));
    };
    const int logical_x = tile.logical_x + map_local(local_x, tile.tile_width, tile.logical_width);
    const int logical_y = tile.logical_y + map_local(local_y, tile.tile_height, tile.logical_height);

    const auto normalize = [](int value, int origin, int extent) {
        if (extent <= 1) return 0;
        const auto relative = std::clamp(value - origin, 0, extent - 1);
        return static_cast<int>((static_cast<std::int64_t>(relative) * 65535 + (extent - 1) / 2) /
                                (extent - 1));
    };
    return {normalize(logical_x, layout.desktop_min_x, layout.desktop_width),
            normalize(logical_y, layout.desktop_min_y, layout.desktop_height)};
}

}
