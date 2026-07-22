cmake_minimum_required(VERSION 3.22)
project(D110Emulator VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.15
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(JUCE)

# Build mt32emu as a static library, C++ interface only (matches rom_test setup).
set(BUILD_SHARED_LIBS FALSE CACHE BOOL "" FORCE)
set(libmt32emu_SHARED FALSE CACHE BOOL "" FORCE)
set(libmt32emu_C_INTERFACE FALSE CACHE BOOL "" FORCE)
set(libmt32emu_CPP_INTERFACE TRUE CACHE BOOL "" FORCE)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../munt/mt32emu" "${CMAKE_CURRENT_BINARY_DIR}/mt32emu_build")

juce_add_plugin(D110Emulator
    COMPANY_NAME "DIY"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD TRUE
    VST3_COPY_DIR "$ENV{LOCALAPPDATA}/Programs/Common/VST3"
    PLUGIN_MANUFACTURER_CODE Manu
    PLUGIN_CODE Dvst
    FORMATS VST3
    PRODUCT_NAME "D-110 Emulator"
)

target_sources(D110Emulator PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
)

target_compile_definitions(D110Emulator PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
)

target_link_libraries(D110Emulator PRIVATE
    MT32Emu::mt32emu
    juce::juce_audio_utils
    juce::juce_audio_processors
)

target_link_libraries(D110Emulator PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)
