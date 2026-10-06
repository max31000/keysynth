cmake_minimum_required(VERSION 3.25)
project(keysynth VERSION 0.1.0 LANGUAGES C CXX)

option(KS_BUILD_TESTS "Build Catch2 tests (ks-tests)" ON)
option(KS_WITH_SFIZZ "Build the sfizz-based sampler (Phase 2, placeholder)" OFF)
# Real-time checks (RtScope + operator new/delete hook + ks::Mutex assert). Cost: one thread_local read per
# allocation. ON by default in every config so the Release test run also exercises them (ARCHITECTURE §4.8).
option(KS_RT_CHECKS "Enable audio-thread real-time violation checks" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# One runtime for everything (deps included): static MSVC runtime.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# Binaries: build/bin/<Config>/
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(Dependencies)

add_subdirectory(engine)
