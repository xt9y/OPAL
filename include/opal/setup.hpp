#pragma once
#include <opal/media.hpp>
namespace opal {
int interactive_run(const StreamOptions &stream={});
int interactive_setup();
int interactive_select();
int interactive_remove();
}
