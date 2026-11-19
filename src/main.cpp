#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "legacy_extended_view.h"
#include "runtime.h"
#if defined(__ANDROID__)
#include <android/log.h>
#include <SDL.h>
#endif

#if defined(GBAGAME_RECOMP_UI)
#include "game_launcher_boot.h"
#endif

namespace {

void print_usage() {
    std::printf(
        "DragonBallZLegacyOfGokuRecomp [--bios <path>] [--rom <path>] "
        "[game_europe.toml]\n");
}

}  // namespace

#if defined(__ANDROID__)
extern "C" int SDL_main(int argc, char** argv) {
#else
int main(int argc, char** argv) {
#endif
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 ||
            std::strcmp(argv[i], "-h") == 0) {
            print_usage();
            return 0;
        }
    }

    gbarecomp::RunOptions opts;
    opts.builtin_game_name = "Dragon Ball Z: The Legacy of Goku";
    opts.builtin_rom_sha1 = "c2691355247b03083578071d4d4a017f5599be20";
    opts.builtin_rom_crc32 = 0xCA6E149Cu;
    opts.mod_game_id = "dragon-ball-z-legacy-of-goku-us";
    opts.mod_owns_adaptive_view = true;
    opts.max_view_width = 480;
    opts.max_resize_view_width = 480;
    opts.resize_driven_view = true;
#if defined(__ANDROID__)
    // AYN Thor: panoramico sin depender del catalogo de mods USA.
    opts.mod_owns_adaptive_view = false;

    // Usamos el mando fisico, sin botones virtuales.
    opts.touch_pad_default = 0;
#endif
    opts.extended_view_init = &legacy::install_extended_view;
    opts.presentation_request = [](gbarecomp::RunOptions::PresentationRequest* request) {
        request->native_view = !legacy::has_authored_view();
        request->black_bars = request->native_view;
    };
    opts.launcher_expose_widescreen = false;
    opts.launcher_expose_adaptive_view = false;
    opts.launcher_region = "USA";
    opts.launcher_game_config = "game_europe.toml";

#if defined(GBAGAME_RECOMP_UI)
    std::vector<std::string> args(argv, argv + argc);
    if (game_launcher_preboot(args, opts)) return 0;
    std::vector<char*> av;
    av.reserve(args.size());
    for (auto& arg : args) av.push_back(arg.data());
    return gbarecomp::run_game(static_cast<int>(av.size()), av.data(), opts);
#else
#if defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_INFO, "LEGACY1_BOOT",
                        "SDL_main argc=%d", argc);

    for (int i = 0; i < argc; ++i) {
        __android_log_print(ANDROID_LOG_INFO, "LEGACY1_BOOT",
                            "argv[%d]=%s", i,
                            argv[i] ? argv[i] : "(null)");
    }

    const char* internal = SDL_AndroidGetInternalStoragePath();
    if (!internal) {
        __android_log_print(ANDROID_LOG_ERROR, "LEGACY1_BOOT",
                            "No se encontro almacenamiento interno");
        return 1;
    }

    std::string base(internal);
    // Temporary acceptance capture using existing runtime diagnostics.
    std::freopen((base + "/legacy1-accept-stderr.log").c_str(), "w", stderr);
    std::freopen((base + "/legacy1-accept-stdout.log").c_str(), "w", stdout);
    setvbuf(stderr, nullptr, _IONBF, 0);
    setvbuf(stdout, nullptr, _IONBF, 0);
    SDL_setenv("GBARECOMP_STRICT_STATIC", "0", 1);
    SDL_setenv("GBARECOMP_SELFHEAL_RECOMPILE", "0", 1);
    SDL_setenv("GBARECOMP_COVERAGE_JSON", (base + "/legacy1-coverage.json").c_str(), 1);

    std::vector<std::string> launch_args = {
        "legacy1",
        "--config", base + "/variants/legacy1-europe/game.toml",
        "--rom", base + "/roms/legacy1-europe.gba",
        "--bios", base + "/bios/gba_bios.bin",
        "--fullscreen=1",
        "--resize-view"
    };

    std::vector<char*> launch_argv;
    for (auto& arg : launch_args)
        launch_argv.push_back(arg.data());

    for (const auto& arg : launch_args)
        __android_log_print(ANDROID_LOG_INFO, "LEGACY1_BOOT",
                            "ARG: %s", arg.c_str());

    int result = gbarecomp::run_game(
        static_cast<int>(launch_argv.size()),
        launch_argv.data(), opts);

    __android_log_print(ANDROID_LOG_ERROR, "LEGACY1_BOOT",
                        "run_game returned %d", result);
    return result;
#else
    return gbarecomp::run_game(argc, argv, opts);
#endif
#endif
}
