cmake_minimum_required(VERSION 3.20)
project(SuperMario64DSRecomp VERSION 0.0.0 LANGUAGES CXX)

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

# This deliberately builds the real ndsrecomp ARM core and recompiler, while
# the per-game target remains selection-only until game bank emission exists.
add_subdirectory(
    "${NDSRECOMP_ROOT}/recompiler"
    "${CMAKE_BINARY_DIR}/ndsrecomp-recompiler")

add_executable(supermario64dsrecomp
    src/main.cpp
    "${NDSRECOMP_ROOT}/recompiler/support/sha1.cpp")
set_target_properties(supermario64dsrecomp PROPERTIES
    OUTPUT_NAME "SuperMario64DSRecomp")
target_include_directories(supermario64dsrecomp PRIVATE
    "${NDSRECOMP_ROOT}/recompiler/support")

# Building this target proves that the pinned ndsrecomp recompiler and the
# SM64DS selector compile together. It does not generate or execute game code.
add_dependencies(supermario64dsrecomp nds_recompile)

if(NOT MSVC)
    target_compile_options(supermario64dsrecomp PRIVATE -Wall -Wextra)
endif()
