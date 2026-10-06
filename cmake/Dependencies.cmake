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

# --- JUCE (bundles the ASIO SDK since 8.0.x; GPLv3/AGPL use) -------------------------------------------
FetchContent_Declare(juce
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.15
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
    ${juce_KS_DIRS})

# --- nlohmann_json --------------------------------------------------------------------------------------
FetchContent_Declare(nlohmann_json
    URL      https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    ${nlohmann_json_KS_DIRS})
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")

# --- IXWebSocket (no TLS, no zlib) ----------------------------------------------------------------------
FetchContent_Declare(ixwebsocket
    GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
    GIT_TAG        v11.4.6
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
        GIT_TAG        v3.8.1
        GIT_SHALLOW    TRUE
        ${catch2_KS_DIRS})
    set(CATCH_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
    set(CATCH_INSTALL_EXTRAS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(Catch2)
endif()

# --- sfizz: Phase 2 (isolated target, warnings off). Placeholder only. ----------------------------------
if(KS_WITH_SFIZZ)
    message(WARNING "KS_WITH_SFIZZ=ON: sfizz integration is not implemented yet (Phase 2); ignoring.")
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
