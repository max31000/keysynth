# Third-party dependencies. Every dependency is pinned to a tag.
# FETCHCONTENT_BASE_DIR is set by scripts/configure.ps1 to <repo>/.deps so build dirs/worktrees share downloads.
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)
# Reuse already-populated sources in the shared .deps cache without re-contacting the network.
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)
# Only *sources* live in the shared cache; build/sub-build dirs stay per build tree (parallel-safe).
macro(ks_dep_dirs name)
    set(${name}_KS_DIRS BINARY_DIR ${CMAKE_BINARY_DIR}/_deps/${name}-build
                        SUBBUILD_DIR ${CMAKE_BINARY_DIR}/_deps/${name}-subbuild)
endmacro()
foreach(_d juce nlohmann_json ixwebsocket catch2)
    ks_dep_dirs(${_d})
endforeach()
# Pins (used by the declarations below and by the shared-source check).
set(KS_PIN_juce 8.0.15)
set(KS_PIN_nlohmann_json 3.12.0)
set(KS_PIN_ixwebsocket v11.4.6)
set(KS_PIN_catch2 v3.8.1)
set(KS_PIN_sfizz 1.2.3)
# A fresh build dir has no populate stamps, so FetchContent would wipe and re-clone the *shared* source dir
# (breaking every other build tree using it). Already-populated shared sources that are at the pinned version are
# used as-is instead; after a pin bump the check fails and FetchContent updates the source dir normally.
find_package(Git QUIET)
foreach(_d juce nlohmann_json ixwebsocket catch2 sfizz)
    string(TOUPPER ${_d} _D)
    set(_src "${FETCHCONTENT_BASE_DIR}/${_d}-src")
    if(DEFINED FETCHCONTENT_SOURCE_DIR_${_D} OR NOT FETCHCONTENT_BASE_DIR OR NOT EXISTS "${_src}/CMakeLists.txt")
        continue()
    endif()
    set(_at_pin FALSE)
    if(_d STREQUAL "nlohmann_json") # URL download: the version is in the amalgamated header banner
        file(STRINGS "${_src}/include/nlohmann/json.hpp" _banner LIMIT_COUNT 5 REGEX "version ${KS_PIN_${_d}}")
        if(_banner)
            set(_at_pin TRUE)
        endif()
    elseif(GIT_FOUND)
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${_src}" describe --tags --exact-match HEAD
                        OUTPUT_VARIABLE _tag OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE _rc)
        if(_rc EQUAL 0 AND _tag STREQUAL KS_PIN_${_d})
            set(_at_pin TRUE)
        endif()
    endif()
    if(_at_pin)
        set(FETCHCONTENT_SOURCE_DIR_${_D} "${_src}")
    else()
        message(STATUS "${_d}: shared source is not at ${KS_PIN_${_d}}; FetchContent will update it")
    endif()
endforeach()

# --- JUCE (bundles the ASIO SDK since 8.0.x; GPLv3/AGPL use) -------------------------------------------
FetchContent_Declare(juce
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        ${KS_PIN_juce}
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
    ${juce_KS_DIRS})

# --- nlohmann_json --------------------------------------------------------------------------------------
FetchContent_Declare(nlohmann_json
    URL      https://github.com/nlohmann/json/releases/download/v${KS_PIN_nlohmann_json}/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    ${nlohmann_json_KS_DIRS})
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")

# --- IXWebSocket (no TLS, no zlib) ----------------------------------------------------------------------
FetchContent_Declare(ixwebsocket
    GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
    GIT_TAG        ${KS_PIN_ixwebsocket}
    GIT_SHALLOW    TRUE
    ${ixwebsocket_KS_DIRS})
set(USE_TLS OFF CACHE BOOL "" FORCE)
set(USE_ZLIB OFF CACHE BOOL "" FORCE)
set(USE_WS OFF CACHE BOOL "" FORCE)
set(USE_TEST OFF CACHE BOOL "" FORCE)
set(IXWEBSOCKET_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(juce nlohmann_json ixwebsocket)

# --- Catch2 v3 (tests only) -----------------------------------------------------------------------------
if(KS_BUILD_TESTS)
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        ${KS_PIN_catch2}
        GIT_SHALLOW    TRUE
        ${catch2_KS_DIRS})
    set(CATCH_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
    set(CATCH_INSTALL_EXTRAS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(Catch2)
endif()

# --- sfizz (SFZ sampler; BSD-2). Isolated static target, warnings off, static MSVC runtime. ----------------
# Release tag pinned; submodules (abseil, simde, dr_libs, ...) fetched recursively.
if(KS_WITH_SFIZZ)
    ks_dep_dirs(sfizz)
    FetchContent_Declare(sfizz
        GIT_REPOSITORY https://github.com/sfztools/sfizz.git
        GIT_TAG        ${KS_PIN_sfizz}
        GIT_SHALLOW    TRUE
        GIT_SUBMODULES_RECURSE TRUE
        GIT_PROGRESS   TRUE
        ${sfizz_KS_DIRS})
    foreach(_opt SFIZZ_JACK SFIZZ_RENDER SFIZZ_SHARED SFIZZ_TESTS SFIZZ_BENCHMARKS SFIZZ_USE_SNDFILE SFIZZ_LV2
                 SFIZZ_LV2_UI SFIZZ_VST SFIZZ_AU SFIZZ_PUREDATA SFIZZ_DEMOS SFIZZ_DEVTOOLS SFIZZ_SPLIT_DEBUG)
        set(${_opt} OFF CACHE BOOL "" FORCE)
    endforeach()
    set(SFIZZ_STATIC_DEPENDENCIES OFF CACHE BOOL "" FORCE)
    set(ENABLE_LTO OFF CACHE BOOL "" FORCE)
    # Runtime: sfizz sets CMAKE_MSVC_RUNTIME_LIBRARY to the same static "MultiThreaded$<$<CONFIG:Debug>:Debug>" we use.
    set(_ks_saved_testing ${BUILD_TESTING})
    set(BUILD_TESTING OFF)
    set(_ks_saved_std ${CMAKE_CXX_STANDARD})
    set(CMAKE_CXX_STANDARD 17) # sfizz targets C++17; scoped to its subdirectory (restored below)
    FetchContent_MakeAvailable(sfizz)
    set(CMAKE_CXX_STANDARD ${_ks_saved_std})
    set(BUILD_TESTING ${_ks_saved_testing})
    if(NOT TARGET sfizz_static)
        message(FATAL_ERROR "sfizz did not provide the sfizz_static target")
    endif()
    # Local fix (idempotent, applied to the shared source tree): sfizz 1.2.3 ADSREnvelope never enters Release when
    # the EG delay segment (delay / delay_random) ends exactly on a segment boundary (delay == 0, not < 0), which
    # leaves notes stuck forever (seen with VPO `delay_random`). Covered by test_sampler "[sampler]".
    set(_ks_adsr "${sfizz_SOURCE_DIR}/src/sfizz/ADSREnvelope.cpp")
    file(READ "${_ks_adsr}" _ks_adsr_src)
    string(FIND "${_ks_adsr_src}" "releaseDelay == 0 && delay < 0" _ks_adsr_bug)
    if(_ks_adsr_bug GREATER -1)
        string(REPLACE "releaseDelay == 0 && delay < 0" "releaseDelay == 0 && delay <= 0" _ks_adsr_src "${_ks_adsr_src}")
        file(WRITE "${_ks_adsr}" "${_ks_adsr_src}")
        message(STATUS "sfizz: patched ADSREnvelope release-after-delay bug")
    elseif(NOT _ks_adsr_src MATCHES "releaseDelay == 0 && delay <= 0")
        message(WARNING "sfizz: ADSREnvelope patch target not found (sfizz bumped?) - re-check the stuck-note fix")
    endif()
    # Every target sfizz (and its bundled deps) defines: warnings off, IDE folder "deps/sfizz".
    function(ks_quiet_targets dir)
        get_property(_targets DIRECTORY ${dir} PROPERTY BUILDSYSTEM_TARGETS)
        foreach(_t ${_targets})
            get_target_property(_type ${_t} TYPE)
            if(NOT _type STREQUAL "INTERFACE_LIBRARY" AND NOT _type STREQUAL "UTILITY")
                if(MSVC)
                    target_compile_options(${_t} PRIVATE /W0)
                endif()
                set_target_properties(${_t} PROPERTIES FOLDER "deps/sfizz")
            endif()
        endforeach()
        get_property(_subdirs DIRECTORY ${dir} PROPERTY SUBDIRECTORIES)
        foreach(_s ${_subdirs})
            ks_quiet_targets(${_s})
        endforeach()
    endfunction()
    ks_quiet_targets(${sfizz_SOURCE_DIR})
    # sfizz force-enables asserts and DBG() (std::cerr) in release builds; both would print/trap on the audio
    # thread. Strip them so Release follows NDEBUG.
    foreach(_t sfizz_parser sfizz_messaging sfizz_internal)
        foreach(_prop COMPILE_DEFINITIONS INTERFACE_COMPILE_DEFINITIONS)
            get_target_property(_defs ${_t} ${_prop})
            if(_defs)
                list(REMOVE_ITEM _defs SFIZZ_ENABLE_RELEASE_ASSERT=1 SFIZZ_ENABLE_RELEASE_DBG=1)
                set_target_properties(${_t} PROPERTIES ${_prop} "${_defs}")
            endif()
        endforeach()
    endforeach()
endif()

# Dependency targets: never warnings-as-errors, quiet.
foreach(_dep ixwebsocket Catch2 Catch2WithMain)
    if(TARGET ${_dep})
        if(MSVC)
            target_compile_options(${_dep} PRIVATE /W0)
        endif()
        set_target_properties(${_dep} PROPERTIES FOLDER "deps")
    endif()
endforeach()
