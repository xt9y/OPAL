#pragma once

#include <opal/display_backend.hpp>

namespace opal {

// Duplicate reuses the existing desktop whenever one is available. Extend uses
// an independent virtual output. Selecting a topology must never require
// changing an existing display's mode.
constexpr bool capture_physical_for_topology(HostDisplayMode mode,
                                             bool physical_available,
                                             bool force_virtual) noexcept
{
    return mode == HostDisplayMode::Duplicate && physical_available && !force_virtual;
}

constexpr DisplayMode virtual_mode_for_topology(HostDisplayMode mode,
                                                const DisplayMode& physical_mode,
                                                const DisplayMode& stream_mode,
                                                bool physical_available) noexcept
{
    if (mode == HostDisplayMode::Duplicate && physical_available)
        return physical_mode;
    return stream_mode;
}

}
