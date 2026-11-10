cmake_minimum_required(VERSION 3.22)

project(MicFxHost VERSION 0.1.0 LANGUAGES C CXX)

# Some Command Line Tools installs ship a clang whose built-in libc++ header directory only holds a
# few sub-directories, so even `#include <algorithm>` fails. The complete set lives in the SDK.
include(CheckCXXSourceCompiles)
check_cxx_source_compiles("#include <algorithm>\nint main() { return 0; }" MFX_STD_HEADERS_FOUND)

if(NOT MFX_STD_HEADERS_FOUND)
    set(sdk_path "${CMAKE_OSX_SYSROOT}")

    if(NOT sdk_path)
        execute_process(COMMAND xcrun --show-sdk-path OUTPUT_VARIABLE sdk_path OUTPUT_STRIP_TRAILING_WHITESPACE)
    endif()

    set(libcxx_flags "-nostdinc++ -isystem ${sdk_path}/usr/include/c++/v1")
    set(CMAKE_REQUIRED_FLAGS "${libcxx_flags}")
    unset(MFX_STD_HEADERS_FOUND_WITH_SDK CACHE)
    check_cxx_source_compiles("#include <algorithm>\nint main() { return 0; }" MFX_STD_HEADERS_FOUND_WITH_SDK)
    unset(CMAKE_REQUIRED_FLAGS)

    if(NOT MFX_STD_HEADERS_FOUND_WITH_SDK)
        message(FATAL_ERROR "The C++ standard library headers cannot be found, even in ${sdk_path}")
    endif()

    message(STATUS "Using libc++ headers from the SDK: ${sdk_path}/usr/include/c++/v1")
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:SHELL:${libcxx_flags}>")
    # JUCE builds its own helper tool (juceaide) in a separate CMake run that only inherits the environment.
    set(ENV{CXXFLAGS} "$ENV{CXXFLAGS} ${libcxx_flags}")
endif()

add_subdirectory(JUCE)

juce_add_gui_app(MicFxHost
    PRODUCT_NAME "mic-fx-host"
    BUNDLE_ID local.mic-fx-host
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "マイク入力にエフェクトを掛けるため、マイクへのアクセスが必要です。"
    NEEDS_WEB_BROWSER FALSE
    NEEDS_CURL FALSE)

target_sources(MicFxHost PRIVATE
    src/Main.cpp
    src/Chain.cpp
    src/Settings.cpp
    src/CommandLine.cpp
    src/MainComponent.cpp)

target_compile_definitions(MicFxHost PRIVATE
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_AU=0
    JUCE_PLUGINHOST_VST=0
    JUCE_PLUGINHOST_LADSPA=0
    JUCE_PLUGINHOST_LV2=0
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:MicFxHost,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:MicFxHost,JUCE_VERSION>")

target_link_libraries(MicFxHost PRIVATE
    juce::juce_audio_utils
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

enable_testing()

set(MFX_TEST_PLUGINS
    "/Library/Audio/Plug-Ins/VST3/TDR Nova.vst3"
    "/Library/Audio/Plug-Ins/VST3/RX 11 Voice De-noise.vst3"
    CACHE STRING "Plugins that ctest loads through --selftest (missing ones are skipped)")

foreach(plugin IN LISTS MFX_TEST_PLUGINS)
    if(EXISTS "${plugin}")
        get_filename_component(plugin_name "${plugin}" NAME_WE)
        add_test(NAME "selftest ${plugin_name}" COMMAND MicFxHost --selftest "${plugin}")
    endif()
endforeach()
