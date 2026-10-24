#pragma once
#include <cstdint>

namespace ps2 {
// Display-only options, independent of emulated GS state and save states.
// bit 0: edge antialiasing.
void set_video_options(std::uint32_t options);
std::uint32_t video_options();
}
