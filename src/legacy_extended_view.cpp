#include "legacy_extended_view.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>

#include "gba_bus.h"
#include "gba_ppu.h"
#include "runtime_bus_bridge.h"

extern "C" unsigned g_ws_active;
extern "C" unsigned g_ws_extra_left;
extern "C" unsigned g_ws_extra_right;

namespace legacy {
namespace {

// ALGP world: 240x160-pixel authored pages, 30x20 tile entries (16-bit).
// ROM scene descriptor from IWRAM 030005FC:
//   +00: pages_w and pages_h (u16 each)
//   +20/+24/+28: ROM pointer to a row-major [pages_w*pages_h] table
//                 of 1200-byte authored tiles, one table for BG1/2/3.
// Confirmed against ALGP snapshot: all 1800 visible VRAM tiles match.
constexpr int kPageW = 240;
constexpr int kPageH = 160;
constexpr int kTilesW = 30;
constexpr int kTilesH = 20;
constexpr int kTilesPerPage = kTilesW * kTilesH;
constexpr std::uint32_t kPageBytes = kTilesPerPage * 2u;
constexpr std::uint32_t kSceneHeader = 0x030005ECu;
constexpr std::uint32_t kSceneDescriptor = 0x030005FCu;
constexpr std::uint32_t kCameraAddress = 0x03000604u;
constexpr std::uint32_t kLiveSources = 0x03004AC4u;
constexpr int kMaxPages = 32 * 32;

struct Field {
    gba::GbaBus* bus = nullptr;
    int last_y = -1000;
    int pages_w = 0;
    int pages_h = 0;
    int camera_x = 0;
    int camera_y = 0;
    std::uint32_t descriptor = 0;
    bool valid = false;
    bool mapped = false;
    std::array<std::array<const std::uint8_t*, kMaxPages>, 3> page{};
};

Field g_field;

gba::GbaBus* bus_now() { return gbarecomp::active_bus(); }

std::uint16_t read_iw16(gba::GbaBus* bus, std::uint32_t addr) {
    if (!bus || addr < 0x03000000u || addr > 0x03007FFEu)
        return 0;
    const auto* mem = bus->iwram_ptr() + (addr - 0x03000000u);
    return static_cast<std::uint16_t>(mem[0]) |
        (static_cast<std::uint16_t>(mem[1]) << 8);
}
std::uint32_t read_iw32(gba::GbaBus* bus, std::uint32_t addr) {
    return std::uint32_t(read_iw16(bus, addr)) |
        (std::uint32_t(read_iw16(bus, addr + 2u)) << 16);
}

bool rom_range(gba::GbaBus* bus, std::uint32_t addr, std::uint32_t bytes) {
    if (!bus || !bus->rom_ptr() || addr < 0x08000000u)
        return false;
    const std::uint64_t offset = std::uint64_t(addr - 0x08000000u);
    return offset + bytes <= bus->rom_size();
}
std::uint32_t read_rom32(gba::GbaBus* bus, std::uint32_t addr) {
    if (!rom_range(bus, addr, 4u)) return 0;
    const auto* p = bus->rom_ptr() + (addr - 0x08000000u);
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
        (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}

bool is_overworld(gba::GbaBus* bus) {
    if (!bus) return false;
    if ((bus->io().read16(0x000u) & 0x1FC7u) != 0x1F40u)
        return false;
    for (int bg = 0; bg < 4; ++bg) {
        const std::uint16_t cnt =
            bus->io().read16(0x008u + std::uint32_t(2 * bg));
        if (((cnt >> 8) & 31u) != static_cast<unsigned>(28 + bg) ||
            ((cnt >> 14) & 3u) != 0u)
            return false;
    }
    return true;
}

int wrap_delta(int a, int b) {
    return ((a - b + 128) & 255) - 128;
}

// The pointers are for the *whole* world. Unlike the four live IWRAM page
// pointers they remain valid before/after the camera crosses a page boundary.
// This is essential for true widescreen while walking.
bool map_scene(gba::GbaBus* bus, std::uint32_t descriptor, int w, int h) {
    if (!rom_range(bus, descriptor, 0x2Cu)) return false;
    const int count = w * h;
    if (w < 1 || h < 1 || w > 32 || h > 32 || count > kMaxPages)
        return false;

    std::array<std::array<const std::uint8_t*, kMaxPages>, 3> next{};
    for (int bg = 0; bg < 3; ++bg) {
        const std::uint32_t table =
            read_rom32(bus, descriptor + 0x20u + std::uint32_t(bg * 4));
        if (!rom_range(bus, table, std::uint32_t(count * 4)))
            return false;
        for (int p = 0; p < count; ++p) {
            const std::uint32_t src =
                read_rom32(bus, table + std::uint32_t(p * 4));
            if (rom_range(bus, src, kPageBytes))
                next[bg][p] = bus->rom_ptr() + (src - 0x08000000u);
        }
    }
    // Confirm native 2x2 page pointers belong to the selected world table.
    // Check up to 12, accepting a few pointers mid-update during movement.
    const int camera_x = read_iw16(bus, kCameraAddress);
    const int camera_y = read_iw16(bus, kCameraAddress + 2u);
    const int base_x = camera_x / kPageW;
    const int base_y = camera_y / kPageH;
    int matched = 0, checked = 0;
    for (int bg = 0; bg < 3; ++bg) {
        for (int row = 0; row < 2; ++row) {
            for (int col = 0; col < 2; ++col) {
                const int x = base_x + col;
                const int y = base_y + row;
                if (x < 0 || x >= w || y < 0 || y >= h) continue;
                const std::uint32_t live = read_iw32(bus,
                    kLiveSources + std::uint32_t(bg * 16 + (row * 2 + col) * 4));
                const std::uint8_t* source = next[bg][y * w + x];
                if (!source || !rom_range(bus, live, kPageBytes)) continue;
                ++checked;
                if (source == bus->rom_ptr() + (live - 0x08000000u))
                    ++matched;
            }
        }
    }
    if (checked < 3 || matched * 4 < checked * 3)
        return false;

    g_field.page = next;
    g_field.descriptor = descriptor;
    g_field.pages_w = w;
    g_field.pages_h = h;
    g_field.mapped = true;
    g_field.valid = true;
    std::fprintf(stderr,
        "[legacy:algp-world-v4] scene %08x mapped %dx%d, native %d/%d sources\n",
        descriptor, w, h, matched, checked);
    return true;
}

void update_field(gba::GbaBus* bus, int scan_y) {
    const bool new_bus = g_field.bus != bus;
    const bool new_frame = new_bus || scan_y < g_field.last_y;
    g_field.bus = bus;
    g_field.last_y = scan_y;
    if (!new_frame) return;
    if (!g_ws_active || !is_overworld(bus)) {
        g_field.valid = false;
        return;
    }

    const int w = read_iw16(bus, kSceneHeader);
    const int h = read_iw16(bus, kSceneHeader + 2u);
    const std::uint32_t descriptor = read_iw32(bus, kSceneDescriptor);
    const int raw_x = read_iw16(bus, kCameraAddress);
    const int raw_y = read_iw16(bus, kCameraAddress + 2u);
    if (!descriptor || w < 1 || h < 1 || w > 32 || h > 32 ||
        raw_x >= w * kPageW || raw_y >= h * kPageH) {
        g_field.valid = false;
        return;
    }

    if (new_bus || !g_field.mapped || g_field.descriptor != descriptor ||
        g_field.pages_w != w || g_field.pages_h != h) {
        g_field.valid = false;
        g_field.mapped = false;
        if (!map_scene(bus, descriptor, w, h)) return;
    }

    const int hofs = bus->io().read16(0x018u) & 255;
    const int vofs = bus->io().read16(0x01Au) & 255;
    const int dx = wrap_delta(hofs, raw_x & 255);
    const int dy = wrap_delta(vofs, raw_y & 255);
    // The ROM may update camera position and BG scroll on separate cycles.
    // Follow the *displayed* scroll for small discrepancies; never discard
    // already mapped scenery simply because Goku has taken another step.
    g_field.camera_x = raw_x + ((dx >= -32 && dx <= 32) ? dx : 0);
    g_field.camera_y = raw_y + ((dy >= -32 && dy <= 32) ? dy : 0);
    g_field.valid = g_field.mapped;
}

extern "C" int legacy_tilemap_provider(
    int bg, int hardware_x, int screen_y, std::uint16_t* out_entry) {
    gba::GbaBus* bus = bus_now();
    if (!out_entry || !g_ws_active || bg < 1 || bg > 3 ||
        !is_overworld(bus))
        return gba::kWsTilemapUnavailable;
    update_field(bus, screen_y);
    if (!g_field.valid) return gba::kWsTilemapUnavailable;
    const int ref_h = bus->io().read16(0x018u) & 255;
    const int ref_v = bus->io().read16(0x01Au) & 255;
    const int own_h = bus->io().read16(0x010u + std::uint32_t(bg * 4)) & 255;
    const int own_v = bus->io().read16(0x012u + std::uint32_t(bg * 4)) & 255;
    const int wx = g_field.camera_x + hardware_x + wrap_delta(own_h, ref_h);
    const int wy = g_field.camera_y + screen_y + wrap_delta(own_v, ref_v);
    if (wx < 0 || wy < 0 || wx >= g_field.pages_w * kPageW ||
        wy >= g_field.pages_h * kPageH)
        return gba::kWsTilemapUnavailable;
    const int page_idx = (wy / kPageH) * g_field.pages_w + wx / kPageW;
    const auto* data = g_field.page[bg - 1][page_idx];
    if (!data) return gba::kWsTilemapUnavailable;
    const int tile_idx = ((wy % kPageH) / 8) * kTilesW + (wx % kPageW) / 8;
    *out_entry = std::uint16_t(data[tile_idx * 2]) |
        std::uint16_t(data[tile_idx * 2 + 1] << 8);
    return gba::kWsTilemapReplace;
}

int object_guard() {
    const int width = 240 + static_cast<int>(g_ws_extra_left) +
        static_cast<int>(g_ws_extra_right);
    return std::max(0, std::min(128, (510 - width) / 2));
}

extern "C" int legacy_obj_x_provider(
    int, std::uint16_t, std::uint16_t attr1, std::uint16_t, int* out_x) {
    if (!out_x || !g_ws_active || !is_overworld(bus_now())) return 0;
    const int raw = attr1 & 0x1FFu;
    // Crucial: never move an arbitrary group of OAM indices to a different
    // origin. HP bar border/fill, changing damage indicators, dialogue face
    // and all other UI pieces must retain the SAME native GBA coordinate
    // system (the PPU automatically centres it at g_ws_extra_left).
    // Only decode signed/offscreen world coordinates; no x-offset is added.
    const int emitted_right = 240 + static_cast<int>(g_ws_extra_right) +
        object_guard();
    *out_x = (raw <= emitted_right) ? raw : raw - 512;
    return 1;
}

extern "C" int legacy_view_rom_read16(
    std::uint32_t address, std::uint16_t original, std::uint16_t* out) {
    if (!out || !g_ws_active) return 0;
    if (address != 0x08011E14u && address != 0x08011E1Cu &&
        address != 0x08011E18u && address != 0x08011E20u)
        return 0;
    if (!is_overworld(bus_now())) return 0;
    const int guard = object_guard();
    switch (address) {
        case 0x08011E14u:
        case 0x08011E1Cu:
            if (original != 0u) return 0;
            *out = static_cast<std::uint16_t>(
                -static_cast<int>(g_ws_extra_left) - guard);
            return 1;
        case 0x08011E18u:
        case 0x08011E20u:
            if (original != 240u) return 0;
            *out = static_cast<std::uint16_t>(
                240 + static_cast<int>(g_ws_extra_right) + guard);
            return 1;
        default:
            return 0;
    }
}

}  // namespace

bool has_authored_view() { return is_overworld(bus_now()); }

void install_extended_view(std::uint32_t, std::uint32_t) {
    g_field = {};
    gba::g_ws_tilemap_provider = &legacy_tilemap_provider;
    gba::g_ws_authored_margin_layers = 1;
    gba::g_ws_bg_x_provider = nullptr;
    gba::g_ws_bg_x_provider_layers = 0;
    gba::g_ws_obj_attr_x_provider = &legacy_obj_x_provider;
    gba::g_ws_obj_native_clip = 0;
    gba::g_rom_read16_override = &legacy_view_rom_read16;
    std::fprintf(stderr,
        "[legacy:algp-world-v4] full ROM world-page tables; unified native UI/OBJ anchors\n");
}

}  // namespace legacy
