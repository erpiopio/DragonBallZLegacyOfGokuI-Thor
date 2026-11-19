#include "legacy1_lower_state.h"
#if defined(__ANDROID__)
extern "C" void legacy1_ra_memory_frame(
    const std::uint8_t*,
    const std::uint8_t*,
    const std::uint8_t*,
    std::size_t);
#endif

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <mutex>
#include <utility>

#if !defined(LEGACY1_LOWER_DECODE_ONLY)
#include "gba_bus.h"
#include "runtime_bus_bridge.h"
#endif
#if defined(__ANDROID__)
#include <jni.h>
#endif

namespace legacy1::lower {
namespace {
constexpr std::uint32_t kRomBase = 0x08000000u;
std::uint16_t u16(const std::uint8_t* p) {
    return std::uint16_t(p[0]) | (std::uint16_t(p[1]) << 8);
}
std::uint32_t u32(const std::uint8_t* p) {
    return std::uint32_t(u16(p)) | (std::uint32_t(u16(p + 2)) << 16);
}
const std::uint8_t* rom_view(const Memory& m, std::uint32_t address,
                             std::size_t bytes) {
    if (!m.rom || address < kRomBase) return nullptr;
    const std::size_t offset = address - kRomBase;
    return offset <= m.rom_size && bytes <= m.rom_size - offset
        ? m.rom + offset : nullptr;
}
const std::uint8_t* ram_view(const Memory& m,std::uint32_t address,std::size_t bytes) {
    if(m.iwram && address>=0x03000000u && address<=0x03008000u &&
       bytes<=0x03008000u-address) return m.iwram+(address-0x03000000u);
    if(m.ewram && address>=0x02000000u && address<=0x02040000u &&
       bytes<=0x02040000u-address) return m.ewram+(address-0x02000000u);
    return nullptr;
}
bool algp(const Memory& m) {
    return m.iwram && m.ewram && m.rom && m.rom_size == 0x800000u &&
           std::memcmp(m.rom + 0xAC, "ALGP", 4) == 0 && m.rom[0xBC] == 0;
}
bool overworld(const Memory& m) {
    if ((m.dispcnt & 0x1FC7u) != 0x1F40u) return false;
    for (int bg = 0; bg < 4; ++bg)
        if (((m.bgcnt[bg] >> 8) & 31u) != unsigned(28 + bg) ||
            ((m.bgcnt[bg] >> 14) & 3u) != 0u) return false;
    return true;
}
std::int32_t color32(std::uint16_t c) {
    const unsigned r = (c & 31u) * 255u / 31u;
    const unsigned g = ((c >> 5) & 31u) * 255u / 31u;
    const unsigned b = ((c >> 10) & 31u) * 255u / 31u;
    return static_cast<std::int32_t>(0xFF000000u | (r << 16) | (g << 8) | b);
}
void utf8(std::string& out, unsigned code) {
    if (code < 0x80) out.push_back(static_cast<char>(code));
    else if (code < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
        out.push_back(static_cast<char>(0x80 | (code & 63)));
    }
}
std::string rom_text(const Memory& m, std::uint32_t address) {
    std::string out;
    for (unsigned i = 0; i < 256; ++i) {
        const auto* p = rom_view(m, address + i, 1);
        if (!p || !*p) break;
        unsigned code = *p;
        // Verified ALGP latin glyph indices; the ROM is not Latin-1.
        switch (code) {
            case 0x88: code=0xC1; break; case 0x8D: code=0x153; break;
            case 0x9B: code=0xE1; break; case 0x9D: code=0xE4; break;
            case 0x9E: code=0xE7; break; case 0xA0: code=0xE9; break;
            case 0xA1: code=0xEA; break; case 0xA6: code=0xED; break;
            case 0xA9: code=0xF1; break; case 0xAB: code=0xF3; break;
            case 0xB1: code=0xFC; break;
            default: if(code >= 0x80) code='?'; break;
        }
        if (code == 10 || code == 13) code=' ';
        if (code >= 32) utf8(out, code);
    }
    return out;
}
}

State decode_state(const Memory& m, std::uint32_t generation) {
    State out;
    out.fill(-1);
    out[0] = 1;
    out[1] = 0;
    out[3] = static_cast<std::int32_t>(generation & 0x7FFFFFFFu);
    if (!algp(m)) return out;
    out[1] = 1;
    // 08003C5E reads this byte; 08003D06..08003D4A wraps it 0..4.
    // Table 08010C80: English, Francais, Deutsch, Italiano, Espanol.
    if (m.iwram[0x20] <= 4) out[2] = m.iwram[0x20];
    // ALGP stores XP and zero-based level separately from the live entity.
    // 08005D64 displays index+1; 08005DA0..08005DBA reads the 16-byte
    // stat-table row. The current-player chain is [030007B4]+3C.
    const auto* player_context=ram_view(m,u32(m.iwram+0x7B4),0x40);
    const auto* actor=player_context ? ram_view(m,u32(player_context+0x3C),0x5C) : nullptr;
    if(actor) {
        out[13] = u16(actor + 0x20);
        out[14] = u16(actor + 0x22);
        // Temporary ALGP position diagnostics; never writes guest RAM.
        out[20] = static_cast<std::int32_t>(u32(player_context + 0x3C));
        out[21] = static_cast<std::int32_t>(u32(m.iwram + 0x5FC));
        out[22] = u16(m.ewram + 0x38A0);
        out[23] = u16(m.ewram + 0x38A2);
        out[24] = u16(m.iwram + 0x604);
        out[25] = u16(m.iwram + 0x606);
        out[26] = u16(m.iwram + 0x608);
        out[27] = u16(m.iwram + 0x60A);
        out[28] = u16(m.iwram + 0x5EC);
        out[29] = u16(m.iwram + 0x5EE);
        for (unsigned i = 0; i < 46; ++i)
            out[32 + i] = u16(actor + i * 2);
        const auto level=u32(m.iwram+0x984);
        const auto xp=u32(m.iwram+0x980);
        // 25 playable rows (display levels1..25), followed by a zero sentinel.
        if(level<25) {
            const auto* row=rom_view(m,0x08011188u+level*16u,32);
            if(row && u32(row+4)>0 && u32(row+4)<=1000000 &&
               u32(row+8)>0 && u32(row+8)<=1000000 && xp<=0x7FFFFFFFu) {
                out[4]=static_cast<int>(level)+1;
                out[5]=static_cast<int>(xp);
                // Original status0800141E calls080008FE: saved frame bias
                // plus running counter, modulo32bits; ROM divides by60.
                const std::uint32_t elapsed=u32(m.iwram+0x44)+u32(m.iwram+0x3B68);
                out[11]=static_cast<std::int32_t>(elapsed/60u);
                const auto threshold=u32(row+16);
                out[6]=threshold==0 ? 0 : static_cast<int>(threshold>xp ? threshold-xp : 0);
                const auto hp=u32(actor+0x4C),maxhp=u32(actor+0x50);
                const auto ki=u32(actor+0x54),maxki=u32(actor+0x58);
                // Reject transient/dangling actor layouts during boot/loads.
                if(maxhp==u32(row+4) && hp<=maxhp) { out[7]=hp; out[8]=maxhp; }
                if(maxki==u32(row+8) && ki<=maxki) { out[9]=ki; out[10]=maxki; }
            }
        }
    }
    // ALGP quest diary: original ROM logic uses two independent bitsets.
    // 03000028: discovered; 03000038: completed (75 quest slots).
    for (unsigned i = 0; i < 75; ++i) {
        const unsigned bit = 1u << (i & 7u);
        const bool discovered = (m.iwram[0x28u + i / 8u] & bit) != 0;
        const bool completed  = (m.iwram[0x38u + i / 8u] & bit) != 0;
        out[128 + i] = completed ? 2 : (discovered ? 1 : 0);
    }
    // Inventory: the original menu reads 34 item-count bytes at 0300004E
    // and groups items by the 34-byte ROM category table 081F41F4.
    // Only 12 categories (0..11) have verified localized menu labels.
    const auto* item_group = rom_view(m, 0x081F41F4u, 34);
    if (item_group) {
        // Categoría del consumible nº 11, verificado por diagnóstico.
        out[114] = item_group[11];
        for (unsigned i = 0; i < 12; ++i) out[203 + i] = 0;
        for (unsigned i = 0; i < 34; ++i) {
            const unsigned group = item_group[i];
            if (group < 12) out[203 + group] += m.iwram[0x4Eu + i];
        }
    }
    // LEGACY1_ITEM_RAM_SNAPSHOT: read-only inventory diagnostics.
    // Slots 80..113 are unused in the current state contract.
    for (unsigned i = 0; i < 34; ++i)
        out[80 + i] = m.iwram[0x4E + i];

    // Guest's skill-selection display at 08001A66 checks these bits.
    // 0 Ki Blast, 1 Kamehameha, 2 Solar Flare.
    const std::uint8_t unlocked = m.iwram[0x4D];
    for (unsigned i = 0; i < 3; ++i)
        out[215 + i] = (unlocked >> i) & 1u;

    // LEGACY1_ATTACK_EQUIP_V1
    // Confirmed by ALGP THUMB at 080035C8:
    // selected attack = [030007A8 + 5].
    const unsigned selected_attack = m.iwram[0x7AD];
    out[115] = selected_attack < 3 &&
        (unlocked & (1u << selected_attack))
        ? static_cast<int>(selected_attack) : -1;
    out[19] = overworld(m) ? 1 : 0;
    if (out[19]) {
        // The stable v4 proves these are the current whole-world descriptor
        // and camera origin. Player coordinates are decoded separately.
        const auto descriptor = u32(m.iwram + 0x5FC);
        if (rom_view(m, descriptor, 0x2C))
            out[12] = static_cast<std::int32_t>(descriptor);
    }
    return out;
}


// LEGACY1_VERIFIED_HERB
// Ejecutar únicamente desde el hilo de emulación.
// La prueba real confirmó: inventario[11] -1, HP +25% del máximo.
bool consume_verified_consumable(const Memory& m, int category) {
    if (!algp(m) || !overworld(m) ||
        (category != 2 && category != 5))
        return false;

    const auto* groups = rom_view(m, 0x081F41F4u, 34);
    if (!groups) return false;

    auto* counts = const_cast<std::uint8_t*>(m.iwram);

    unsigned slot = 34;
    if (category == 2) {
        // All three herb inventory slots belong to category 2.
        for (unsigned i = 11; i <= 13; ++i) {
            if (groups[i] == 2 && counts[0x4E + i] > 0) {
                slot = i;
                break;
            }
        }
    } else if (groups[19] == 5 && counts[0x4E + 19] > 0) {
        slot = 19;
    }

    if (slot == 34) return false;

    const auto* context = ram_view(
        m, u32(m.iwram + 0x7B4), 0x40);
    const auto* actor = context
        ? ram_view(m, u32(context + 0x3C), 0x5C)
        : nullptr;

    if (!actor) return false;

    const auto level = u32(m.iwram + 0x984);
    const auto* stats = level < 25
        ? rom_view(m, 0x08011188u + level * 16u, 32)
        : nullptr;

    if (!stats) return false;

    const std::uint32_t hp = u32(actor + 0x4C);
    const std::uint32_t max_hp = u32(actor + 0x50);
    const std::uint32_t ki = u32(actor + 0x54);
    const std::uint32_t max_ki = u32(actor + 0x58);

    if (!max_hp || !max_ki ||
        max_hp != u32(stats + 4) ||
        max_ki != u32(stats + 8) ||
        hp > max_hp || ki > max_ki)
        return false;

    std::uint32_t new_hp = hp;
    std::uint32_t new_ki = ki;

    if (category == 2) {
        // Herb: restore 25% maximum HP.
        if (hp >= max_hp) return false;
        const auto amount = max_hp / 4u;
        if (!amount) return false;
        new_hp = hp > max_hp - amount
            ? max_hp : hp + amount;
    } else {
        // Senzu Bean: fully restore HP AND Ki.
        if (hp >= max_hp && ki >= max_ki)
            return false;
        new_hp = max_hp;
        new_ki = max_ki;
    }

    auto* writable = const_cast<std::uint8_t*>(actor);

    for (unsigned i = 0; i < 4; ++i) {
        writable[0x4C + i] =
            static_cast<std::uint8_t>(new_hp >> (8u * i));
        writable[0x54 + i] =
            static_cast<std::uint8_t>(new_ki >> (8u * i));
    }

    --counts[0x4E + slot];
    return true;
}

bool equip_unlocked_attack(const Memory& m, int attack) {
    if (!algp(m) || attack < 0 || attack >= 3)
        return false;

    const unsigned unlocked = m.iwram[0x4D];
    const unsigned current = m.iwram[0x7AD];

    // Never equip a locked attack or write during invalid guest state.
    if (current >= 3 || !(unlocked & (1u << attack)))
        return false;

    // Exact byte modified by the original ALGP selection routine.
    const_cast<std::uint8_t*>(m.iwram)[0x7AD] =
        static_cast<std::uint8_t>(attack);
    return true;
}

std::array<std::string, 75> decode_journal(const Memory& m) {
    std::array<std::string, 75> out{};
    if (!algp(m) || m.iwram[0x20]>4) return out;
    const auto* table_ptr=rom_view(m,0x08569F50u + m.iwram[0x20]*4,4);
    if (!table_ptr) return out;
    const auto strings=u32(table_ptr);
    for (unsigned quest=0; quest<75; ++quest) {
        const auto* descriptor_ptr=rom_view(m,0x08569E24u+quest*4,4);
        if (!descriptor_ptr) continue;
        const auto* descriptor=rom_view(m,u32(descriptor_ptr),1);
        if (!descriptor || !descriptor[0]) continue;
        const auto* text_ptr=rom_view(m,strings+descriptor[0]*4u,4);
        if (text_ptr) out[quest]=rom_text(m,u32(text_ptr));
    }
    return out;
}

std::array<std::string, 32> decode_inventory_names(const Memory& m) {
    std::array<std::string, 32> out{};
    if (!algp(m) || m.iwram[0x20] > 4) return out;
    const unsigned language = m.iwram[0x20];
    // ALGP original five-language table; entries are ROM text pointers.
    const auto* language_ptr = rom_view(m, 0x085EA164u + language * 4u, 4);
    const auto* indexes = rom_view(m, 0x081F2A90u, 24);
    if (!language_ptr || !indexes) return out;
    const std::uint32_t strings = u32(language_ptr);
    for (unsigned i = 0; i < 24; ++i) {
        const auto* text_ptr = rom_view(m, strings + indexes[i] * 4u, 4);
        if (text_ptr) out[i] = rom_text(m, u32(text_ptr));
    }
    // Same indices used by the guest skill-list routine at 08001A48.
    constexpr unsigned skill_indexes[] = {17, 18, 19};
    for (unsigned i = 0; i < 3; ++i) {
        const auto* text_ptr = rom_view(m, strings + skill_indexes[i] * 4u, 4);
        if (text_ptr) out[24 + i] = rom_text(m, u32(text_ptr));
    }
    return out;
}

std::vector<std::int32_t> decode_map(const Memory& m) {
    if (!algp(m) || !overworld(m) || !m.vram || !m.palette) return {};
    // ALGP's authored pages are 240x160, 30x20 16-bit tile entries.
    // This is the same ROM layout proven by the stable v4, decoded here
    // without calling or changing the primary-screen renderer.
    const int pages_w = u16(m.iwram + 0x5EC);
    const int pages_h = u16(m.iwram + 0x5EE);
    if (pages_w < 1 || pages_w > 32 || pages_h < 1 || pages_h > 32) return {};
    const auto descriptor = u32(m.iwram + 0x5FC);
    const auto* scene = rom_view(m, descriptor, 0x2C);
    if (!scene || u16(scene) != pages_w || u16(scene + 2) != pages_h) return {};
    const int world_w = pages_w * 240, world_h = pages_h * 160;
    const int count = pages_w * pages_h;
    std::array<std::vector<const std::uint8_t*>, 3> pages;
    for (int bg = 0; bg < 3; ++bg) {
        const auto* table = rom_view(m, u32(scene + 0x20 + bg * 4), count * 4u);
        if (!table) return {};
        pages[bg].resize(count);
        for (int page = 0; page < count; ++page)
            pages[bg][page] = rom_view(m, u32(table + page * 4), 1200u);
    }
    const int width = world_w >= world_h ? 256 : std::max(1, world_w * 256 / world_h);
    const int height = world_h >= world_w ? 256 : std::max(1, world_h * 256 / world_w);
    std::vector<std::int32_t> out(4 + width * height);
    out[0] = width; out[1] = height; out[2] = world_w; out[3] = world_h;
    const auto backdrop = color32(u16(m.palette));
    std::array<int, 3> order{{1, 2, 3}};
    // Draw furthest first. Lower BG index wins ties on GBA.
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        const int pa = m.bgcnt[a] & 3, pb = m.bgcnt[b] & 3;
        return pa != pb ? pa > pb : a > b;
    });
    for (int y = 0; y < height; ++y) {
        const int wy = (2 * y + 1) * world_h / (2 * height);
        for (int x = 0; x < width; ++x) {
            const int wx = (2 * x + 1) * world_w / (2 * width);
            const int page = (wy / 160) * pages_w + wx / 240;
            const int tile = ((wy % 160) / 8) * 30 + (wx % 240) / 8;
            std::int32_t color = backdrop;
            for (const int bg : order) {
                if (!(m.dispcnt & (0x100u << bg))) continue;
                const auto* map = pages[bg - 1][page];
                if (!map) continue;
                const auto entry = u16(map + tile * 2);
                const auto cnt = m.bgcnt[bg];
                int px = wx & 7, py = wy & 7;
                if (entry & 0x400u) px = 7 - px;
                if (entry & 0x800u) py = 7 - py;
                const bool eight = (cnt & 0x80u) != 0;
                const unsigned charbase = ((cnt >> 2) & 3u) * 0x4000u;
                const unsigned addr = charbase + (entry & 0x3FFu) * (eight ? 64u : 32u) +
                    py * (eight ? 8u : 4u) + (eight ? px : px / 2);
                if (addr >= 0x10000u) continue;
                unsigned index = m.vram[addr];
                if (!eight) index = (index >> ((px & 1) * 4)) & 15u;
                if (!index) continue;
                if (!eight) index |= ((entry >> 12) & 15u) << 4;
                color = color32(u16(m.palette + index * 2));
            }
            out[4 + y * width + x] = color;
        }
    }
    return out;
}
}

#if !defined(LEGACY1_LOWER_DECODE_ONLY)
namespace {
std::mutex publication_mutex;
legacy1::lower::State publication = [] {
    legacy1::lower::State state; state.fill(-1); state[0] = 1; state[1] = 0;
    return state;
}();
std::vector<std::int32_t> map_publication;
std::array<std::string,75> journal_publication;
std::array<std::string,32> inventory_publication;
std::uint32_t generation = 0;
std::atomic<int> pending_inventory_category{-1};
std::atomic<int> pending_attack{-1};
std::int32_t map_candidate = -1;
std::int32_t map_ready = -1;
unsigned map_frames = 0;
int last_language = -1;
}

extern "C" void legacy1_lower_publish_frame() {
    auto* bus = gbarecomp::active_bus();
    legacy1::lower::Memory memory;
    if (bus) {
        memory.iwram = bus->iwram_ptr(); memory.ewram = bus->ewram_ptr();
        memory.rom = bus->rom_ptr(); memory.rom_size = bus->rom_size();
        memory.vram = bus->vram_ptr(); memory.palette = bus->pal_ptr();
        memory.dispcnt = bus->io().read16(0);
        for (int bg = 0; bg < 4; ++bg)
            memory.bgcnt[bg] = bus->io().read16(8 + bg * 2);
    }
    const int requested_category =
        pending_inventory_category.exchange(-1, std::memory_order_acq_rel);

    if (requested_category >= 0)
        legacy1::lower::consume_verified_consumable(memory, requested_category);

    const int requested_attack =
        pending_attack.exchange(-1, std::memory_order_acq_rel);

    if (requested_attack >= 0)
        legacy1::lower::equip_unlocked_attack(
            memory, requested_attack);

    auto next = legacy1::lower::decode_state(memory, ++generation);
#if defined(__ANDROID__)
    legacy1_ra_memory_frame(
        memory.iwram, memory.ewram,
        memory.rom, memory.rom_size);
#endif
    const std::int32_t scene = next[19] == 1 ? next[12] : -1;
    bool update_map = false;
    std::vector<std::int32_t> new_map;

    if (scene != map_candidate) {
        map_candidate = scene;
        map_ready = -1;
        map_frames = 0;
        update_map = true;
    }

    if (scene > 0 && map_ready != scene) {
        ++map_frames;
        if (map_frames >= 45 &&
            (map_frames - 45) % 60 == 0) {
            new_map = legacy1::lower::decode_map(memory);
            if (new_map.size() > 4) {
                map_ready = scene;
                update_map = true;
            }
        }
    }

    next[16] = map_ready;

    // Retain the independent Scouter/map implementation from the user's
    // in-progress branch. Do not change map_publication or decode_map here.
    // Cache translated ROM labels per language, on the guest emulation thread;
    // the Android view only reads immutable published copies.
    const int locale = next[1] == 1 ? next[2] : -1;
    if (locale != last_language) {
        auto new_journal = locale >= 0 ? legacy1::lower::decode_journal(memory)
            : std::array<std::string, 75>{};
        auto new_items = locale >= 0 ? legacy1::lower::decode_inventory_names(memory)
            : std::array<std::string, 32>{};
        std::lock_guard<std::mutex> lock(publication_mutex);
        publication = next;
        if (update_map)
            map_publication = std::move(new_map);
        journal_publication = std::move(new_journal);
        inventory_publication = std::move(new_items);
        last_language = locale;
    } else {
        std::lock_guard<std::mutex> lock(publication_mutex);
        publication = next;
        if (update_map)
            map_publication = std::move(new_map);
    }
}

#if defined(__ANDROID__)
namespace {
jintArray make_ints(JNIEnv* env, const std::int32_t* values, std::size_t count) {
    auto out = env->NewIntArray(static_cast<jsize>(count));
    if (out && count) env->SetIntArrayRegion(out, 0, static_cast<jsize>(count), values);
    return out;
}
template<std::size_t N>
jobjectArray make_strings(JNIEnv* env, const std::array<std::string,N>& strings) {
    const auto type=env->FindClass("java/lang/String");
    if (!type) return nullptr;
    auto out=env->NewObjectArray(static_cast<jsize>(N),type,nullptr);
    env->DeleteLocalRef(type);
    if(!out) return nullptr;
    for(jsize i=0;i<static_cast<jsize>(N);++i) {
        auto text=env->NewStringUTF(strings[i].c_str());
        if(!text) return out;
        env->SetObjectArrayElement(out,i,text);
        env->DeleteLocalRef(text);
        if(env->ExceptionCheck()) break;
    }
    return out;
}
}
extern "C" JNIEXPORT jintArray JNICALL
Java_org_erpiopio_legacygoku1_thor_Legacy1Native_readState(JNIEnv* env, jclass) {
    legacy1::lower::State state;
    { std::lock_guard<std::mutex> lock(publication_mutex); state = publication; }
    return make_ints(env, state.data(), state.size());
}
extern "C" JNIEXPORT jintArray JNICALL
Java_org_erpiopio_legacygoku1_thor_Legacy1Native_readMap(JNIEnv* env, jclass) {
    std::vector<std::int32_t> map;
    { std::lock_guard<std::mutex> lock(publication_mutex); map = map_publication; }
    return make_ints(env, map.data(), map.size());
}
extern "C" JNIEXPORT void JNICALL
Java_org_erpiopio_legacygoku1_thor_Legacy1Native_consumeInventoryCategory(
    JNIEnv*, jclass, jint category) {
    if (category >= 0 && category < 12)
        pending_inventory_category.store(
            category, std::memory_order_release);
}

extern "C" JNIEXPORT void JNICALL
Java_org_erpiopio_legacygoku1_thor_Legacy1Native_equipAttack(
    JNIEnv*, jclass, jint attack) {
    if (attack >= 0 && attack < 3)
        pending_attack.store(attack, std::memory_order_release);
}

extern "C" JNIEXPORT jobjectArray JNICALL
Java_org_erpiopio_legacygoku1_thor_Legacy1Native_readJournal(JNIEnv* env, jclass) {
    std::array<std::string,75> strings;
    { std::lock_guard<std::mutex> lock(publication_mutex); strings=journal_publication; }
    return make_strings(env,strings);
}
extern "C" JNIEXPORT jobjectArray JNICALL
Java_org_erpiopio_legacygoku1_thor_Legacy1Native_readInventoryNames(JNIEnv* env, jclass) {
    std::array<std::string,32> strings;
    { std::lock_guard<std::mutex> lock(publication_mutex); strings=inventory_publication; }
    return make_strings(env,strings);
}
#endif
#endif
