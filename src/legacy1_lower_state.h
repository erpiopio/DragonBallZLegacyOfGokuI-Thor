#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace legacy1::lower {
// The Android reader copies only a publication made by the emulation thread.
// Values unavailable for this scene/ROM revision remain -1, never invented.
// 0..126 retains the ORIGINAL Goku/Scouter diagnostic ABI (including 20..77).
// 128..202 journal bits, 203..214 inventory category counts, 215..217 skills.
// Keep existing consumers' original offsets unchanged when extending the ABI.
constexpr std::size_t kStateSize = 224;
using State = std::array<std::int32_t, kStateSize>;
struct Memory {
    const std::uint8_t* iwram = nullptr;
    const std::uint8_t* ewram = nullptr;
    const std::uint8_t* rom = nullptr;
    std::size_t rom_size = 0;
    const std::uint8_t* vram = nullptr;
    const std::uint8_t* palette = nullptr;
    std::array<std::uint16_t, 4> bgcnt{};
    std::uint16_t dispcnt = 0;
};
State decode_state(const Memory& memory, std::uint32_t generation);
// Header: width,height,worldWidth,worldHeight; then width*height ARGB pixels.
std::vector<std::int32_t> decode_map(const Memory& memory);
std::array<std::string, 75> decode_journal(const Memory& memory);
// 0..23: singular/plural names, 24..26: learnable attacks (ROM-language)
std::array<std::string, 32> decode_inventory_names(const Memory& memory);
}

// Optional weak engine seam. Called only at the guest frame boundary.
extern "C" void legacy1_lower_publish_frame();
