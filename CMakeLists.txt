cmake_minimum_required(VERSION 3.22)

project(Wirevox VERSION 0.1.0 LANGUAGES C CXX)

# The product name lives only here. It becomes the .app name, the window title, the settings folder
# and, through MFX_PRODUCT_NAME below, the constant in src/Product.h.
set(MFX_PRODUCT_NAME "WIREVOX")
string(TOLOWER "${MFX_PRODUCT_NAME}" MFX_PRODUCT_ID)

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

juce_add_gui_app(Wirevox
    PRODUCT_NAME "${MFX_PRODUCT_NAME}"
    BUNDLE_ID local.nil.${MFX_PRODUCT_ID}
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.png"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "${MFX_PRODUCT_NAME} がマイク入力にエフェクトを掛けるため、マイクへのアクセスが必要です。"
    NEEDS_WEB_BROWSER FALSE
    NEEDS_CURL FALSE)

target_sources(Wirevox PRIVATE
    src/Main.cpp
    src/Chain.cpp
    src/ProcessTime.cpp
    src/Breakdown.cpp
    src/Settings.cpp
    src/CommandLine.cpp
    src/Correlate.cpp
    src/Hal.cpp
    src/Monitor.cpp
    src/HostCallback.cpp
    src/Engine.cpp
    src/LatencyTuner.cpp
    src/Measure.cpp
    src/SelfTests.cpp
    src/MeasureCommand.cpp
    src/BreakdownPanel.cpp
    src/MainComponent.cpp
    src/Theme.cpp
    src/ThemeRegistry.cpp
    src/Rack84Theme.cpp
    src/TerminalTheme.cpp
    src/Desktop95Theme.cpp
    src/NeonTheme.cpp)

target_compile_definitions(Wirevox PRIVATE
    MFX_PRODUCT_NAME="${MFX_PRODUCT_NAME}"
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_AU=0
    JUCE_PLUGINHOST_VST=0
    JUCE_PLUGINHOST_LADSPA=0
    JUCE_PLUGINHOST_LV2=0
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:Wirevox,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:Wirevox,JUCE_VERSION>")

target_link_libraries(Wirevox PRIVATE
    juce::juce_audio_utils
    juce::juce_dsp
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
        add_test(NAME "${MFX_PRODUCT_NAME} selftest ${plugin_name}" COMMAND Wirevox --selftest "${plugin}")
    endif()
endforeach()

add_test(NAME "${MFX_PRODUCT_NAME} settings migration and theme" COMMAND Wirevox --selftest-settings)
add_test(NAME "${MFX_PRODUCT_NAME} delay estimator" COMMAND Wirevox --selftest-correlate)
add_test(NAME "${MFX_PRODUCT_NAME} hard bypass" COMMAND Wirevox --selftest-chain)
add_test(NAME "${MFX_PRODUCT_NAME} sweep choice" COMMAND Wirevox --selftest-sweep)
add_test(NAME "${MFX_PRODUCT_NAME} latency timing and burst injection" COMMAND Wirevox --selftest-timing)
add_test(NAME "${MFX_PRODUCT_NAME} engine persistence, MIN LATENCY and restore" COMMAND Wirevox --selftest-engine)
add_test(NAME "${MFX_PRODUCT_NAME} latency breakdown and saved excess" COMMAND Wirevox --selftest-breakdown)
add_test(NAME "${MFX_PRODUCT_NAME} per-plugin processing time" COMMAND Wirevox --selftest-processtime)
add_test(NAME "${MFX_PRODUCT_NAME} monitor output" COMMAND Wirevox --selftest-monitor)
