cmake_minimum_required(VERSION 3.20)
project(SuperMario64DSRecomp VERSION 0.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Match the sibling-framework layout used by the SNES and PSX game repos. The
# local `ndsrecomp` entry is an ignored junction to ../ndsrecomp.
set(NDSRECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/ndsrecomp" CACHE PATH
    "Path to the ndsrecomp framework checkout")

if(NOT EXISTS "${NDSRECOMP_ROOT}/recompiler/CMakeLists.txt")
    message(FATAL_ERROR
        "ndsrecomp framework not found at ${NDSRECOMP_ROOT}. "
        "Create the sibling junction or pass -DNDSRECOMP_ROOT=<path>.")
endif()

include("${NDSRECOMP_ROOT}/cmake/NdsCompilerCache.cmake")
include("${NDSRECOMP_ROOT}/cmake/NdsJobPools.cmake")

# Build the real ndsrecomp ARM core/recompiler in the same build graph as the
# per-game executable.
add_subdirectory(
    "${NDSRECOMP_ROOT}/recompiler"
    "${CMAKE_BINARY_DIR}/ndsrecomp-recompiler")

set(SM64DS_ROM "${CMAKE_CURRENT_SOURCE_DIR}/Super Mario 64 DS.nds" CACHE FILEPATH
    "Path to the verified European ASMP revision-0 ROM")
set(SM64DS_DECOMP_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sm64ds-decomp" CACHE PATH
    "Path to the pinned tangosdev/sm64ds-decomp checkout")
set(SM64DS_GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated")
set(SM64DS_INPUT_DIR "${SM64DS_GENERATED_DIR}/inputs")
set(SM64DS_RECOMP_DIR "${SM64DS_GENERATED_DIR}/recomp")
set(SM64DS_HLE_MANIFEST
    "${CMAKE_CURRENT_SOURCE_DIR}/config/sm64ds_hle.toml")
set(SM64DS_BANK_STAMPS)

set(_sm64ds_pinned_python
    "${SM64DS_DECOMP_ROOT}/.venv/Scripts/python.exe")
if(EXISTS "${_sm64ds_pinned_python}")
    set(SM64DS_PYTHON "${_sm64ds_pinned_python}" CACHE FILEPATH
        "Python interpreter with ndspy installed")
else()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(SM64DS_PYTHON "${Python3_EXECUTABLE}" CACHE FILEPATH
        "Python interpreter with ndspy installed")
endif()

set(SM64DS_PREP_OUTPUTS
    "${SM64DS_INPUT_DIR}/arm9.bin"
    "${SM64DS_INPUT_DIR}/arm7.bin"
    "${SM64DS_INPUT_DIR}/arm9.toml"
    "${SM64DS_INPUT_DIR}/arm7.toml")
add_custom_command(
    OUTPUT ${SM64DS_PREP_OUTPUTS}
    COMMAND "${SM64DS_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/prepare_sm64ds.py"
            --rom "${SM64DS_ROM}"
            --decomp "${SM64DS_DECOMP_ROOT}"
            --out "${SM64DS_INPUT_DIR}"
    DEPENDS
        "${SM64DS_ROM}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/prepare_sm64ds.py"
        "${SM64DS_DECOMP_ROOT}/config/arm9/symbols.txt"
        "${SM64DS_DECOMP_ROOT}/config/arm9/delinks.txt"
    COMMENT "Verifying and preparing local SM64DS EU code images")
add_custom_target(sm64ds_prepare DEPENDS ${SM64DS_PREP_OUTPUTS})

set(SM64DS_ARM9_SOURCES)
foreach(shard RANGE 0 15)
    if(shard LESS 10)
        set(shard_name "0${shard}")
    else()
        set(shard_name "${shard}")
    endif()
    list(APPEND SM64DS_ARM9_SOURCES
        "${SM64DS_RECOMP_DIR}/sm64ds_arm9_${shard_name}.c")
endforeach()
list(APPEND SM64DS_ARM9_SOURCES
    "${SM64DS_RECOMP_DIR}/sm64ds_arm9_dispatch.c")
set(SM64DS_ARM7_SOURCES
    "${SM64DS_RECOMP_DIR}/sm64ds_arm7_00.c"
    "${SM64DS_RECOMP_DIR}/sm64ds_arm7_01.c"
    "${SM64DS_RECOMP_DIR}/sm64ds_arm7_dispatch.c")
set(SM64DS_BANK_SOURCES ${SM64DS_ARM9_SOURCES} ${SM64DS_ARM7_SOURCES})

set(SM64DS_ARM9_STAMP "${SM64DS_RECOMP_DIR}/sm64ds_arm9.stamp")
add_custom_command(
    OUTPUT "${SM64DS_ARM9_STAMP}"
    BYPRODUCTS ${SM64DS_ARM9_SOURCES}
        "${SM64DS_RECOMP_DIR}/sm64ds_arm9.h"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${SM64DS_RECOMP_DIR}"
    COMMAND $<TARGET_FILE:nds_recompile>
            --config "${SM64DS_INPUT_DIR}/arm9.toml"
            --bin "${SM64DS_INPUT_DIR}/arm9.bin"
            --out "${SM64DS_RECOMP_DIR}"
            --bank sm64ds_arm9
            --shards 16
            --stable-address-shards
            # The pinned decomp supplies exact sizes; the largest ARM9
            # function is 12,460 bytes. Keeping complete functions avoids a
            # correctness-sensitive mid-function Tier-3 handoff.
            --max-function-bytes 16384
            --validate-live-bytes
            --hle-manifest "${SM64DS_HLE_MANIFEST}"
    COMMAND ${CMAKE_COMMAND} -E touch "${SM64DS_ARM9_STAMP}"
    DEPENDS nds_recompile ${SM64DS_PREP_OUTPUTS} "${SM64DS_HLE_MANIFEST}"
    COMMENT "Statically recompiling the SM64DS ARM9 main module")
list(APPEND SM64DS_BANK_STAMPS "${SM64DS_ARM9_STAMP}")
set(SM64DS_ARM7_STAMP "${SM64DS_RECOMP_DIR}/sm64ds_arm7.stamp")
add_custom_command(
    OUTPUT "${SM64DS_ARM7_STAMP}"
    BYPRODUCTS ${SM64DS_ARM7_SOURCES}
        "${SM64DS_RECOMP_DIR}/sm64ds_arm7.h"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${SM64DS_RECOMP_DIR}"
    COMMAND $<TARGET_FILE:nds_recompile>
            --config "${SM64DS_INPUT_DIR}/arm7.toml"
            --bin "${SM64DS_INPUT_DIR}/arm7.bin"
            --out "${SM64DS_RECOMP_DIR}"
            --bank sm64ds_arm7
            --shards 2
            --max-function-bytes 512
    COMMAND ${CMAKE_COMMAND} -E touch "${SM64DS_ARM7_STAMP}"
    DEPENDS nds_recompile ${SM64DS_PREP_OUTPUTS}
    COMMENT "Statically recompiling the initial SM64DS ARM7 closure")
list(APPEND SM64DS_BANK_STAMPS "${SM64DS_ARM7_STAMP}")

# The ARM7 runtime-RAM bank: the game relocates its steady-state ARM7 code
# (sound engine + services) into shared WRAM / ARM7 WRAM / high main RAM
# during boot, beyond the ROM-derived closure above. Its source image is a
# guest-produced capture (tools/capture_arm7_ram_bank.py); every generated
# function carries a live-byte validation, so dispatch only fires once the
# guest's own boot has reproduced those bytes. Optional: fresh checkouts
# build without it until the capture has been produced locally.
set(SM64DS_ARM7_RAM_CONFIG
    "${CMAKE_CURRENT_SOURCE_DIR}/config/sm64ds_arm7_ram.toml")
set(SM64DS_ARM7_RAM_BIN
    "${SM64DS_GENERATED_DIR}/capture/sm64ds_arm7_ram.bin")
if(EXISTS "${SM64DS_ARM7_RAM_CONFIG}" AND EXISTS "${SM64DS_ARM7_RAM_BIN}")
    set(SM64DS_ARM7_RAM_SOURCES)
    foreach(shard RANGE 0 15)
        if(shard LESS 10)
            set(shard_name "0${shard}")
        else()
            set(shard_name "${shard}")
        endif()
        list(APPEND SM64DS_ARM7_RAM_SOURCES
            "${SM64DS_RECOMP_DIR}/sm64ds_arm7_ram_${shard_name}.c")
    endforeach()
    list(APPEND SM64DS_ARM7_RAM_SOURCES
        "${SM64DS_RECOMP_DIR}/sm64ds_arm7_ram_dispatch.c")
    set(SM64DS_ARM7_RAM_STAMP
        "${SM64DS_RECOMP_DIR}/sm64ds_arm7_ram.stamp")
    add_custom_command(
        OUTPUT "${SM64DS_ARM7_RAM_STAMP}"
        BYPRODUCTS ${SM64DS_ARM7_RAM_SOURCES}
            "${SM64DS_RECOMP_DIR}/sm64ds_arm7_ram.h"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${SM64DS_RECOMP_DIR}"
        COMMAND $<TARGET_FILE:nds_recompile>
                --config "${SM64DS_ARM7_RAM_CONFIG}"
                --bin "${SM64DS_ARM7_RAM_BIN}"
                --out "${SM64DS_RECOMP_DIR}"
                --bank sm64ds_arm7_ram
                --shards 16
                --stable-address-shards
                --max-function-bytes 512
                --validate-live-bytes
        COMMAND ${CMAKE_COMMAND} -E touch "${SM64DS_ARM7_RAM_STAMP}"
        DEPENDS nds_recompile
                "${SM64DS_ARM7_RAM_CONFIG}"
                "${SM64DS_ARM7_RAM_BIN}"
        COMMENT "Statically recompiling the SM64DS ARM7 runtime-RAM bank")
    list(APPEND SM64DS_BANK_STAMPS "${SM64DS_ARM7_RAM_STAMP}")
    list(APPEND SM64DS_ARM7_SOURCES ${SM64DS_ARM7_RAM_SOURCES})
    list(APPEND SM64DS_BANK_SOURCES ${SM64DS_ARM7_RAM_SOURCES})
endif()

# The ARM9 runtime-RAM bank: the game copies code out of the static closure
# at runtime — ITCM-resident fast paths plus overlays loaded over and past
# the arm9.bin image. Source image is a guest-produced ITCM+main-RAM
# composite (tools/capture_arm9_ram_bank.py, fw_arm9 bank model: the 32 KiB
# ITCM high mirror at 0x01FF8000 immediately precedes main RAM); every
# generated function carries a live-byte validation. Optional like the ARM7
# RAM bank above.
set(SM64DS_ARM9_RAM_CONFIG
    "${CMAKE_CURRENT_SOURCE_DIR}/config/sm64ds_arm9_ram.toml")
set(SM64DS_ARM9_RAM_BIN
    "${SM64DS_GENERATED_DIR}/capture/sm64ds_arm9_ram.bin")
if(EXISTS "${SM64DS_ARM9_RAM_CONFIG}" AND EXISTS "${SM64DS_ARM9_RAM_BIN}")
    set(SM64DS_ARM9_RAM_SOURCES)
    foreach(shard RANGE 0 15)
        if(shard LESS 10)
            set(shard_name "0${shard}")
        else()
            set(shard_name "${shard}")
        endif()
        list(APPEND SM64DS_ARM9_RAM_SOURCES
            "${SM64DS_RECOMP_DIR}/sm64ds_arm9_ram_${shard_name}.c")
    endforeach()
    list(APPEND SM64DS_ARM9_RAM_SOURCES
        "${SM64DS_RECOMP_DIR}/sm64ds_arm9_ram_dispatch.c")
    set(SM64DS_ARM9_RAM_STAMP
        "${SM64DS_RECOMP_DIR}/sm64ds_arm9_ram.stamp")
    add_custom_command(
        OUTPUT "${SM64DS_ARM9_RAM_STAMP}"
        BYPRODUCTS ${SM64DS_ARM9_RAM_SOURCES}
            "${SM64DS_RECOMP_DIR}/sm64ds_arm9_ram.h"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${SM64DS_RECOMP_DIR}"
        COMMAND $<TARGET_FILE:nds_recompile>
                --config "${SM64DS_ARM9_RAM_CONFIG}"
                --bin "${SM64DS_ARM9_RAM_BIN}"
                --out "${SM64DS_RECOMP_DIR}"
                --bank sm64ds_arm9_ram
                --shards 16
                --stable-address-shards
                --max-function-bytes 512
                --validate-live-bytes
        COMMAND ${CMAKE_COMMAND} -E touch "${SM64DS_ARM9_RAM_STAMP}"
        DEPENDS nds_recompile
                "${SM64DS_ARM9_RAM_CONFIG}"
                "${SM64DS_ARM9_RAM_BIN}"
        COMMENT "Statically recompiling the SM64DS ARM9 runtime-RAM bank")
    list(APPEND SM64DS_BANK_STAMPS "${SM64DS_ARM9_RAM_STAMP}")
    list(APPEND SM64DS_ARM9_SOURCES ${SM64DS_ARM9_RAM_SOURCES})
    list(APPEND SM64DS_BANK_SOURCES ${SM64DS_ARM9_RAM_SOURCES})
endif()

# Gameplay replaces large ARM9 overlay ranges that are also hot during the
# title/attract window. Preserve both captured byte generations as distinct
# content-validated banks instead of pairing one union of entry points with
# whichever image happened to be captured last.
set(SM64DS_ARM9_RAM_GAMEPLAY_CONFIG
    "${CMAKE_CURRENT_SOURCE_DIR}/config/sm64ds_arm9_ram_gameplay.toml")
set(SM64DS_ARM9_RAM_GAMEPLAY_BIN
    "${SM64DS_GENERATED_DIR}/capture/sm64ds_arm9_ram_gameplay.bin")
if(EXISTS "${SM64DS_ARM9_RAM_GAMEPLAY_CONFIG}" AND
   EXISTS "${SM64DS_ARM9_RAM_GAMEPLAY_BIN}")
    set(SM64DS_ARM9_RAM_GAMEPLAY_SOURCES)
    foreach(shard RANGE 0 15)
        if(shard LESS 10)
            set(shard_name "0${shard}")
        else()
            set(shard_name "${shard}")
        endif()
        list(APPEND SM64DS_ARM9_RAM_GAMEPLAY_SOURCES
            "${SM64DS_RECOMP_DIR}/sm64ds_arm9_ram_gameplay_${shard_name}.c")
    endforeach()
    list(APPEND SM64DS_ARM9_RAM_GAMEPLAY_SOURCES
        "${SM64DS_RECOMP_DIR}/sm64ds_arm9_ram_gameplay_dispatch.c")
    set(SM64DS_ARM9_RAM_GAMEPLAY_STAMP
        "${SM64DS_RECOMP_DIR}/sm64ds_arm9_ram_gameplay.stamp")
    add_custom_command(
        OUTPUT "${SM64DS_ARM9_RAM_GAMEPLAY_STAMP}"
        BYPRODUCTS ${SM64DS_ARM9_RAM_GAMEPLAY_SOURCES}
            "${SM64DS_RECOMP_DIR}/sm64ds_arm9_ram_gameplay.h"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${SM64DS_RECOMP_DIR}"
        COMMAND $<TARGET_FILE:nds_recompile>
                --config "${SM64DS_ARM9_RAM_GAMEPLAY_CONFIG}"
                --bin "${SM64DS_ARM9_RAM_GAMEPLAY_BIN}"
                --out "${SM64DS_RECOMP_DIR}"
                --bank sm64ds_arm9_ram_gameplay
                --shards 16
                --stable-address-shards
                --max-function-bytes 512
                --validate-live-bytes
        COMMAND ${CMAKE_COMMAND} -E touch
                "${SM64DS_ARM9_RAM_GAMEPLAY_STAMP}"
        DEPENDS nds_recompile
                "${SM64DS_ARM9_RAM_GAMEPLAY_CONFIG}"
                "${SM64DS_ARM9_RAM_GAMEPLAY_BIN}"
        COMMENT "Statically recompiling the SM64DS ARM9 gameplay RAM bank")
    list(APPEND SM64DS_BANK_STAMPS "${SM64DS_ARM9_RAM_GAMEPLAY_STAMP}")
    list(APPEND SM64DS_ARM9_SOURCES ${SM64DS_ARM9_RAM_GAMEPLAY_SOURCES})
    list(APPEND SM64DS_BANK_SOURCES ${SM64DS_ARM9_RAM_GAMEPLAY_SOURCES})
endif()

# Compile the ROM-derived generated banks with the same C ABI as the framework
# runner. They stay ignored and are not linked into the selector until the
# cartridge runtime can actually reach them.
add_custom_target(sm64ds_generate_banks DEPENDS ${SM64DS_BANK_STAMPS})
add_library(sm64ds_recompiled_banks STATIC ${SM64DS_BANK_SOURCES})
add_dependencies(sm64ds_recompiled_banks sm64ds_generate_banks)
ndsrecomp_limit_generated_compiles(sm64ds_recompiled_banks)
ndsrecomp_limit_link(sm64ds_recompiled_banks)
set_source_files_properties(${SM64DS_ARM9_SOURCES} PROPERTIES
    COMPILE_DEFINITIONS "NDS_STATIC_CPU=0")
set_source_files_properties(${SM64DS_ARM7_SOURCES} PROPERTIES
    COMPILE_DEFINITIONS "NDS_STATIC_CPU=1")
target_include_directories(sm64ds_recompiled_banks PRIVATE
    "${SM64DS_RECOMP_DIR}"
    "${NDSRECOMP_ROOT}/recompiler/armv4t")
if(NOT MSVC)
    target_compile_options(sm64ds_recompiled_banks PRIVATE
        -w -O0 -g0)
endif()

add_executable(supermario64dsrecomp
    src/main.cpp
    "${NDSRECOMP_ROOT}/recompiler/support/sha1.cpp")
ndsrecomp_limit_link(supermario64dsrecomp)
set_target_properties(supermario64dsrecomp PROPERTIES
    OUTPUT_NAME "SuperMario64DSRecomp")
target_include_directories(supermario64dsrecomp PRIVATE
    "${NDSRECOMP_ROOT}/recompiler/support")

# Building this target proves that the pinned ndsrecomp recompiler and the
# SM64DS selector compile together. It does not generate or execute game code.
add_dependencies(supermario64dsrecomp nds_recompile)
add_dependencies(supermario64dsrecomp sm64ds_recompiled_banks)

if(NOT MSVC)
    target_compile_options(supermario64dsrecomp PRIVATE -Wall -Wextra)
endif()

# Native recomp-ui launcher. The adaptive camera extension is a title mod;
# physical stacked/separate windows and presentation quality remain ordinary
# Display settings.
option(SM64DS_BUILD_LAUNCHER "Build the SM64DS recomp-ui launcher" ON)
set(SM64DS_RUNNER_EXE
    "${NDSRECOMP_ROOT}/runner/build-sm64-native/nds_runner.exe"
    CACHE FILEPATH "Path to the native nds_runner executable")
if(SM64DS_BUILD_LAUNCHER AND WIN32)
    set(SM64DS_RECOMP_UI_ROOT
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/recomp-ui"
        CACHE PATH "Path to the recomp-ui checkout")
    if(NOT EXISTS "${SM64DS_RECOMP_UI_ROOT}/recomp_ui.cmake")
        message(FATAL_ERROR
            "recomp-ui submodule missing. Run "
            "`git submodule update --init third_party/recomp-ui`.")
    endif()
    find_package(SDL2 CONFIG REQUIRED)
    add_executable(sm64ds_launcher src/launcher_main.cpp)
    target_link_libraries(sm64ds_launcher PRIVATE SDL2::SDL2)
    target_compile_definitions(sm64ds_launcher PRIVATE
        SM64DS_RUNNER_EXE="${SM64DS_RUNNER_EXE}"
        SM64DS_BIOS_DIR="${NDSRECOMP_ROOT}/bios"
        SM64DS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    set(RECOMP_UI_ROOT "${SM64DS_RECOMP_UI_ROOT}")
    set(RECOMP_UI_SDL3 OFF CACHE BOOL "" FORCE)
    set(RECOMP_UI_ENABLE_MODS ON CACHE BOOL "" FORCE)
    include("${SM64DS_RECOMP_UI_ROOT}/recomp_ui.cmake")
    recomp_target_launcher_ui(sm64ds_launcher
        CONSOLE nds
        BOXART "${CMAKE_CURRENT_SOURCE_DIR}/assets/boxart_sm64ds_eu.tga")
endif()
