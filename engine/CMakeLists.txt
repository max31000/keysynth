# keysynth engine. Sources are globbed (CONFIGURE_DEPENDS): adding a module never edits CMake.

# ---------------------------------------------------------------------------------------------------------
# ks_juce: JUCE modules compiled once into a static library (pattern from JUCE "CMake API.md").
add_library(ks_juce STATIC)
target_link_libraries(ks_juce
    PRIVATE juce::juce_core juce::juce_events juce::juce_audio_basics juce::juce_audio_devices
    PUBLIC  juce::juce_recommended_config_flags)
target_compile_definitions(ks_juce
    PUBLIC
        JUCE_ASIO=1
        JUCE_WASAPI=1
        JUCE_DIRECTSOUND=1
        JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0
        JUCE_MODAL_LOOPS_PERMITTED=0
        JUCE_STRICT_REFCOUNTEDPOINTER=1
        JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1
        JUCE_STANDALONE_APPLICATION=1
        DONT_SET_USING_JUCE_NAMESPACE=1
    INTERFACE $<TARGET_PROPERTY:ks_juce,COMPILE_DEFINITIONS>)
target_include_directories(ks_juce SYSTEM INTERFACE $<TARGET_PROPERTY:ks_juce,INCLUDE_DIRECTORIES>)
set_target_properties(ks_juce PROPERTIES FOLDER "deps")
if(MSVC)
    target_compile_options(ks_juce PRIVATE /W0 /bigobj)
endif()

# ---------------------------------------------------------------------------------------------------------
# Warning policy for engine code only (deps are compiled with their own flags).
add_library(ks_warnings INTERFACE)
if(MSVC)
    target_compile_options(ks_warnings INTERFACE
        /W4 /WX /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor /external:W0
        /wd4324   # structure padded due to alignment specifier (intended: cache-line alignment)
    )
    target_compile_definitions(ks_warnings INTERFACE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(ks_warnings INTERFACE -Wall -Wextra -Werror)
endif()

# ---------------------------------------------------------------------------------------------------------
# ks_msfa: vendored DX7 FM core (third_party/msfa, Apache-2.0 + Dexed engines GPL-3.0+), warnings off.
# Only the `fm` instrument includes its headers (SYSTEM, so /W4 /WX doesn't apply to them).
set(KS_MSFA_DIR ${CMAKE_SOURCE_DIR}/third_party/msfa)
file(GLOB KS_MSFA_SOURCES CONFIGURE_DEPENDS ${KS_MSFA_DIR}/*.cc ${KS_MSFA_DIR}/*.cpp ${KS_MSFA_DIR}/dexed/*.cpp)
add_library(ks_msfa STATIC ${KS_MSFA_SOURCES})
target_include_directories(ks_msfa SYSTEM PUBLIC ${KS_MSFA_DIR} ${CMAKE_SOURCE_DIR}/third_party)
set_target_properties(ks_msfa PROPERTIES FOLDER "deps")
if(MSVC)
    target_compile_options(ks_msfa PRIVATE /W0 /utf-8)
    target_compile_definitions(ks_msfa PRIVATE _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
    target_compile_options(ks_msfa PRIVATE -w)
endif()

# ---------------------------------------------------------------------------------------------------------
# ks_core: everything except app/ and tools/.
file(GLOB_RECURSE KS_CORE_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.h)
list(FILTER KS_CORE_SOURCES EXCLUDE REGEX "/src/app/")
add_library(ks_core STATIC ${KS_CORE_SOURCES})
target_include_directories(ks_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(ks_core
    PUBLIC ks_juce nlohmann_json::nlohmann_json ixwebsocket
    PRIVATE ks_warnings ks_msfa)
# Dep headers as SYSTEM so /W4 /WX only applies to our code.
get_target_property(_ix_inc ixwebsocket INTERFACE_INCLUDE_DIRECTORIES)
if(_ix_inc)
    target_include_directories(ks_core SYSTEM PUBLIC ${_ix_inc})
endif()
if(TARGET sfizz_static)
    # sfizz only inside ks_core (instruments/sampler); its headers as SYSTEM so /W4 /WX stays on our code.
    target_link_libraries(ks_core PRIVATE sfizz_static)
    get_target_property(_sfizz_inc sfizz_static INTERFACE_INCLUDE_DIRECTORIES)
    target_include_directories(ks_core SYSTEM PRIVATE ${_sfizz_inc})
    target_compile_definitions(ks_core PUBLIC KS_HAS_SFIZZ=1)
endif()
target_compile_definitions(ks_core PUBLIC
    KS_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    $<$<BOOL:${KS_RT_CHECKS}>:KS_RT_CHECKS=1>)
if(WIN32)
    target_link_libraries(ks_core PUBLIC avrt ws2_32)
endif()
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/src FILES ${KS_CORE_SOURCES})

# ---------------------------------------------------------------------------------------------------------
add_executable(keysynth-engine src/app/main.cpp)
target_link_libraries(keysynth-engine PRIVATE ks_core ks_warnings)

add_executable(ks-render tools/ks_render.cpp)
target_link_libraries(ks-render PRIVATE ks_core ks_warnings)

add_executable(ks-bench tools/ks_bench.cpp)
target_link_libraries(ks-bench PRIVATE ks_core ks_warnings)

if(KS_BUILD_TESTS)
    file(GLOB KS_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
    add_executable(ks-tests ${KS_TEST_SOURCES})
    target_link_libraries(ks-tests PRIVATE ks_core ks_warnings Catch2::Catch2WithMain)
    get_target_property(_catch_inc Catch2 INTERFACE_INCLUDE_DIRECTORIES)
    target_include_directories(ks-tests SYSTEM PRIVATE ${_catch_inc})
    enable_testing()
    add_test(NAME ks-tests COMMAND ks-tests)
endif()
