cmake_minimum_required(VERSION 3.20)
project(DragonBallZLegacyOfGokuRecomp VERSION 0.0.4 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(GBARECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/gbarecomp"
    CACHE PATH "Path to this game's gbarecomp worktree")
if(NOT EXISTS "${GBARECOMP_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR
        "GBARECOMP_ROOT is not a gbarecomp checkout: ${GBARECOMP_ROOT}")
endif()

set(GBARECOMP_RUNTIME_UI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/recomp-ui"
    CACHE PATH "recomp-ui root used by the live GBA runtime adapter")
set(RECOMP_UI_ROOT "${GBARECOMP_RUNTIME_UI_ROOT}"
    CACHE PATH "Path to the shared recomp-ui checkout")

# Android utiliza su propia interfaz, sin ImGui de escritorio.
if(ANDROID)
    set(GBARECOMP_RUNTIME_UI_ROOT "" CACHE PATH
        "Runtime UI root" FORCE)
    set(RECOMP_UI_ROOT "" CACHE PATH
        "recomp-ui root" FORCE)
endif()
set(GBARECOMP_ENABLE_MODS ON CACHE BOOL
    "Enable the built-in Legacy of Goku mod catalog" FORCE)

add_subdirectory("${GBARECOMP_ROOT}" gbarecomp_build EXCLUDE_FROM_ALL)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated/recompiled.cpp")
    message(FATAL_ERROR
        "Stale monolithic generated/recompiled.cpp found. Run tools/regen.ps1.")
endif()
file(GLOB GAME_GENERATED_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/*.cpp")

if(ANDROID)
    set(LEGACY1_TARGET main)
    add_library(${LEGACY1_TARGET} SHARED)
    target_link_libraries(${LEGACY1_TARGET} PRIVATE SDL2::SDL2 log android)
    target_link_options(${LEGACY1_TARGET} PRIVATE "-Wl,--no-undefined")
else()
    set(LEGACY1_TARGET DragonBallZLegacyOfGokuRecomp)
    add_executable(${LEGACY1_TARGET})
endif()

target_sources(${LEGACY1_TARGET} PRIVATE
    src/main.cpp
    src/legacy1_lower_state.cpp
    src/legacy_extended_view.cpp
    src/mods/legacy_adaptive_view_plugin.cpp
    ${GAME_GENERATED_SOURCES})

set(GAME_RECOMPILED_SOURCES ${GAME_GENERATED_SOURCES})
list(FILTER GAME_RECOMPILED_SOURCES INCLUDE REGEX
    "/recompiled_[0-9]+\\.cpp$")
if(GAME_RECOMPILED_SOURCES)
    set_source_files_properties(${GAME_RECOMPILED_SOURCES} PROPERTIES
        COMPILE_OPTIONS "-O1;-g0")
endif()

target_include_directories(${LEGACY1_TARGET} PRIVATE
    "${GBARECOMP_ROOT}/src/armv4t"
    "${GBARECOMP_ROOT}/src/gba"
    "${GBARECOMP_ROOT}/src/runtime"
    "${GBARECOMP_ROOT}/src/debug"
    src)

target_link_libraries(${LEGACY1_TARGET} PRIVATE
    "-Wl,--start-group"
    gbarecomp_runtime
    gbarecomp_debug
    gbarecomp_gba
    gbarecomp_armv4t
    "-Wl,--end-group")

# LEGACY1_RETROACHIEVEMENTS_CMAKE_V1
# Copia privada del cliente oficial. Solo Android.
if(ANDROID)
    set(LEGACY1_RA_ROOT
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/rcheevos")

    file(GLOB LEGACY1_RA_API CONFIGURE_DEPENDS
        "${LEGACY1_RA_ROOT}/src/rapi/*.c")
    file(GLOB LEGACY1_RA_RUNTIME CONFIGURE_DEPENDS
        "${LEGACY1_RA_ROOT}/src/rcheevos/*.c")
    file(GLOB LEGACY1_RA_HASH CONFIGURE_DEPENDS
        "${LEGACY1_RA_ROOT}/src/rhash/*.c")

    add_library(legacy1_rcheevos STATIC
        "${LEGACY1_RA_ROOT}/src/rc_client.c"
        "${LEGACY1_RA_ROOT}/src/rc_client_raintegration.c"
        "${LEGACY1_RA_ROOT}/src/rc_compat.c"
        "${LEGACY1_RA_ROOT}/src/rc_util.c"
        "${LEGACY1_RA_ROOT}/src/rc_version.c"
        ${LEGACY1_RA_API}
        ${LEGACY1_RA_RUNTIME}
        ${LEGACY1_RA_HASH})

    set_target_properties(legacy1_rcheevos PROPERTIES
        POSITION_INDEPENDENT_CODE ON)

    target_include_directories(legacy1_rcheevos PUBLIC
        "${LEGACY1_RA_ROOT}/include")

    target_compile_definitions(legacy1_rcheevos PRIVATE
        RC_CLIENT_SUPPORTS_HASH)

    target_sources(${LEGACY1_TARGET}
        PRIVATE src/legacy1_ra_memory.cpp)
    target_link_libraries(${LEGACY1_TARGET}
        PRIVATE legacy1_rcheevos)
endif()


target_compile_definitions(${LEGACY1_TARGET} PRIVATE
    GBARECOMP_DEFAULT_DEBUG_PORT=19887
    GBARECOMP_WINDOW_TITLE="DragonBallZLegacyOfGokuRecomp"
    GBARECOMP_DEFAULT_GAME_CONFIG="game_europe.toml")

option(GBAGAME_RECOMP_UI "Build the recomp-ui pre-boot launcher" ON)
if(GBAGAME_RECOMP_UI)
    include(${RECOMP_UI_ROOT}/recomp_ui.cmake)
    add_library(game_launcher_ui STATIC src/game_launcher_boot.cpp)
    target_include_directories(game_launcher_ui PRIVATE
        "${GBARECOMP_ROOT}/src/runtime"
        "${GBARECOMP_ROOT}/src/gba"
        src)
    target_compile_definitions(game_launcher_ui PRIVATE
        GBARECOMP_ENABLE_MODS=1)
    recomp_target_launcher_ui(game_launcher_ui
        CONSOLE gba
        BOXART ${CMAKE_CURRENT_SOURCE_DIR}/recomp/launcher/boxart.tga)
    find_package(SDL2 REQUIRED)
    target_link_libraries(game_launcher_ui PUBLIC SDL2::SDL2)
    if(WIN32)
        target_link_libraries(game_launcher_ui PUBLIC opengl32)
    endif()
    target_link_libraries(${LEGACY1_TARGET} PRIVATE game_launcher_ui)
    set_property(SOURCE src/main.cpp APPEND PROPERTY
        COMPILE_DEFINITIONS GBAGAME_RECOMP_UI)
endif()

set(LEGACY_PRELOADED_MODS "${CMAKE_CURRENT_SOURCE_DIR}/mods/preloaded")
if(EXISTS "${LEGACY_PRELOADED_MODS}/packages")
    file(GLOB_RECURSE LEGACY_MOD_MANIFESTS CONFIGURE_DEPENDS
        "${LEGACY_PRELOADED_MODS}/packages/*.toml")
    set_property(TARGET ${LEGACY1_TARGET} APPEND PROPERTY
        LINK_DEPENDS ${LEGACY_MOD_MANIFESTS})
    add_custom_command(TARGET ${LEGACY1_TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${LEGACY_PRELOADED_MODS}"
            "$<TARGET_FILE_DIR:${LEGACY1_TARGET}>/mods"
        COMMENT "Preloading the Legacy of Goku mod catalog")
endif()

if(MSVC)
    target_link_options(DragonBallZLegacyOfGokuRecomp PRIVATE /STACK:16777216)
elseif(MINGW OR WIN32)
    target_link_options(DragonBallZLegacyOfGokuRecomp PRIVATE
        -Wl,--stack,16777216)
endif()
