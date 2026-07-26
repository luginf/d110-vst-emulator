cmake_minimum_required(VERSION 3.22)
project(D110Emulator VERSION 0.9.2)

# C++20 because MAME's headers need it.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# CRITICAL: MAME's Release libs are built against the static runtime, so everything
# here - JUCE and mt32emu included - must use /MT too or the link mixes runtimes.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

include("${CMAKE_CURRENT_SOURCE_DIR}/mame.cmake")

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

# The front panel IS the reference photograph - see docs/panel_reference_notes.md.
juce_add_binary_data(D110PanelData
    SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../docs/panel_reference.png")

juce_add_plugin(D110Emulator
    COMPANY_NAME "DIY"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD TRUE
    VST3_COPY_DIR "C:/Program Files/Common Files/VST3"
    PLUGIN_MANUFACTURER_CODE Manu
    PLUGIN_CODE Dvst
    FORMATS VST3 Standalone
    PRODUCT_NAME "D-110 Emulator"
)

target_sources(D110Emulator PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/D110Core.cpp
    "${MAME_DRIVLIST}"
)

target_include_directories(D110Emulator PRIVATE ${MAME_INCLUDES})
target_compile_definitions(D110Emulator PRIVATE ${MAME_DEFS})

target_compile_definitions(D110Emulator PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
)

target_link_libraries(D110Emulator PRIVATE
    D110PanelData
    ${MAME_LIBS} ${MAME_SYS_LIBS}
    MT32Emu::mt32emu
    juce::juce_audio_utils
    juce::juce_audio_processors
)

# MAME's own emulator_info definitions collide with ours by design, and its libs are
# built with a different debug-info setting than JUCE's.
foreach(_fmt VST3 Standalone)
    if(TARGET D110Emulator_${_fmt})
        target_link_options(D110Emulator_${_fmt} PRIVATE /ignore:4099 /FORCE:MULTIPLE)
    endif()
endforeach()

target_link_libraries(D110Emulator PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

# ---------------------------------------------------------------------------
# Offline audio check: instantiates the real processor, powers it on, plays a note
# and measures the output. No DAW, no audio device. Also the harness for proving
# that a panel edit becomes audible once the RAM bridge is in.
# ---------------------------------------------------------------------------
add_executable(d110_audio_test audio_test.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_audio_test PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_audio_test PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_audio_test PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_audio_test PRIVATE /ignore:4099 /FORCE:MULTIPLE)

# Maps the firmware's battery RAM onto Roland's SysEx addresses by experiment: presses
# panel buttons and reports which bytes moved. See docs/sysex_address_map.md.
add_executable(d110_bridge_probe bridge_probe.cpp Source/D110Core.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_bridge_probe PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_bridge_probe PRIVATE ${MAME_DEFS})
target_link_libraries(d110_bridge_probe PRIVATE ${MAME_LIBS} ${MAME_SYS_LIBS})
target_link_options(d110_bridge_probe PRIVATE /ignore:4099 /FORCE:MULTIPLE)

# ---------------------------------------------------------------------------
# Locates the Tone Temporary Area in the firmware's RAM by comparing bytes: reads the
# tone mt32emu holds for each part back out of the engine and slides it over the whole
# 32 KB. An exact match both pins the base and proves that mirroring an unedited tone
# changes nothing - the null test the bridge needed before the deep Edit pages could be
# wired up. See docs/sysex_address_map.md.
# ---------------------------------------------------------------------------
add_executable(d110_tone_probe tone_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_tone_probe PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_tone_probe PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_tone_probe PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_tone_probe PRIVATE /ignore:4099 /FORCE:MULTIPLE)

# Checks that loading the plugin does NOT re-initialise the instrument, and that the
# hardware's own factory init (latch WRITE/COPY, power on, ENTER) works from the panel.
add_executable(d110_coldstart_test coldstart_test.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_coldstart_test PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_coldstart_test PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_coldstart_test PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_coldstart_test PRIVATE /ignore:4099 /FORCE:MULTIPLE)

# Checks that a project recalls its own firmware memory, that two instances get separate
# memory, and that the second one refuses to power on instead of crashing the host.
add_executable(d110_state_test state_test.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_state_test PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_state_test PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_state_test PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_state_test PRIVATE /ignore:4099 /FORCE:MULTIPLE)

# Isolates why the control board stops responding after some seconds of playing: runs the
# firmware for a minute with and without host MIDI, pressing a button throughout.
add_executable(d110_longrun_test longrun_test.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_longrun_test PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_longrun_test PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_longrun_test PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_longrun_test PRIVATE /ignore:4099 /FORCE:MULTIPLE)

# Records what really happens when two machines run in one process, which is what a DAW
# does as soon as a second instance of the plugin is loaded. Diagnostic, not a fix.
add_executable(d110_two_instance_test two_instance_test.cpp Source/D110Core.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_two_instance_test PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_two_instance_test PRIVATE ${MAME_DEFS})
target_link_libraries(d110_two_instance_test PRIVATE ${MAME_LIBS} ${MAME_SYS_LIBS})
target_link_options(d110_two_instance_test PRIVATE /ignore:4099 /FORCE:MULTIPLE)

# Console check of the D110Core lifecycle: boots the real firmware in-process and prints
# the MSM6222B's own dot matrix while pressing front-panel buttons. Proves the wrap
# independently of the plugin.
add_executable(d110_core_test core_test.cpp Source/D110Core.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_core_test PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_core_test PRIVATE ${MAME_DEFS})
target_link_libraries(d110_core_test PRIVATE ${MAME_LIBS} ${MAME_SYS_LIBS})
target_link_options(d110_core_test PRIVATE /ignore:4099 /FORCE:MULTIPLE)
