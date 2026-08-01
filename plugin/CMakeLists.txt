cmake_minimum_required(VERSION 3.22)
project(D110Emulator VERSION 0.9.9)

# C++20 because MAME's headers need it.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# CRITICAL: MAME's Release libs are built against the static runtime, so everything
# here - JUCE and mt32emu included - must use /MT too or the link mixes runtimes.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

# Комментарии в этом проекте пишутся по-русски, а MSVC без этого ключа читает исходник
# в кодовой странице системы и на кириллице выдаёт C4819 - а в худшем случае съедает
# байт, похожий на обратную косую черту, в конце строки комментария.
if(MSVC)
    add_compile_options(/utf-8)
endif()

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
    SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../docs/panel_reference.png"
            "${CMAKE_CURRENT_SOURCE_DIR}/../docs/memory_card_m256d.png")

juce_add_plugin(D110Emulator
    # Без этого JUCE ставит собственную версию по умолчанию, и хост показывает её вместо
    # той, под которой выпущена сборка. Версия здесь одна и берётся из project() выше.
    VERSION ${PROJECT_VERSION}
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

# Discovers the LA32's register interface by logging every access to an address the D-110's
# map leaves unclaimed - which is precisely where the missing sound board would answer.
add_executable(d110_la32_probe la32_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_la32_probe PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_la32_probe PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_la32_probe PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_la32_probe PRIVATE /ignore:4099 /FORCE:MULTIPLE)

# One-shot: play a single note, freeze it (no interrupt answered), dump the raw voice
# bookkeeping tables - ground truth for what StuckPolicy::La32Stub's slot scan should see.
add_executable(d110_la32_lifecycle la32_lifecycle_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_la32_lifecycle PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_la32_lifecycle PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_la32_lifecycle PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_la32_lifecycle PRIVATE /ignore:4099 /FORCE:MULTIPLE)

# Live write tap over the voice-context bookkeeping arrays (f3c0..f480), engaged
# together with StuckPolicy::La32Stub - answers whether the firmware's own housekeeping
# ever reaches f460[] bit 6 at all (the second stall) instead of guessing from statics.
add_executable(d110_la32_ctx la32_ctx_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_la32_ctx PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_la32_ctx PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_la32_ctx PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_la32_ctx PRIVATE /ignore:4099 /FORCE:MULTIPLE)

# Practical question hang_probe/longrun_test don't ask: with La32Stub engaged, does the
# panel survive NORMAL playing (single notes, realistic gaps), not deliberate chord-stress.
add_executable(d110_la32_realistic la32_realistic_test.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_la32_realistic PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_la32_realistic PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_la32_realistic PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_la32_realistic PRIVATE /ignore:4099 /FORCE:MULTIPLE)

# Reproduces the user's own reported steps directly - EDIT+ENTER into ROM Play, ENTER to
# play the demo song - instead of only inferring from synthetic MIDI tests that it hits
# the same freeze. Also checks whether La32Stub fares differently here, since the demo
# song runs entirely on MAME's own thread with no host MIDI injection at all.
add_executable(d110_demo_song_repro demo_song_repro.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_demo_song_repro PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_demo_song_repro PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_demo_song_repro PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_demo_song_repro PRIVATE /ignore:4099 /FORCE:MULTIPLE)

# Does a factory reset actually restore the documented per-part pan fan, and does that
# byte reach the sound engine as a real stereo shift? Answers the "panning feels wrong /
# it won't return to defaults" question by measurement rather than by listening.
add_executable(d110_pan_verify pan_verify.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_pan_verify PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_pan_verify PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_pan_verify PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_pan_verify PRIVATE /ignore:4099 /FORCE:MULTIPLE)

# The demo song is silent because the firmware plays it internally and never transmits it.
# This checks whether the notes are nevertheless recoverable from the firmware's own RAM
# (f400 = note, f420 = velocity, f3a0 = part?), which would let the bridge carry them.
add_executable(d110_note_source note_source_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_note_source PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_note_source PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_note_source PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_note_source PRIVATE /ignore:4099 /FORCE:MULTIPLE)

# Renders the demo song to a WAV so "is there any sound at all" is answered by a file the
# user can listen to, not by inference.
add_executable(d110_demo_wav demo_wav_render.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_demo_wav PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_demo_wav PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_demo_wav PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_demo_wav PRIVATE /ignore:4099 /FORCE:MULTIPLE)

# Sounds each part on its own and measures it, so "part 6 lights up but makes no noise" is
# answered per part instead of by ear across nine at once. Two passes: straight into the
# engine, and through the firmware on that part's own channel.
add_executable(d110_part_audio part_audio_test.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_part_audio PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_part_audio PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_part_audio PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_part_audio PRIVATE /ignore:4099 /FORCE:MULTIPLE)

# Renders the demo song once per part, with only that part delivered, and measures what
# comes out. Note counts cannot tell "silent" from "buried"; a level can.
add_executable(d110_solo_level solo_level_test.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_solo_level PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_solo_level PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_solo_level PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_solo_level PRIVATE /ignore:4099 /FORCE:MULTIPLE)

# Compares, per part and DURING the demo, what the firmware holds against what the engine
# holds. A level 20 dB adrift is either the bridge failing to carry something or the
# arrangement; only a byte-for-byte comparison tells them apart.
add_executable(d110_part_state part_state_compare.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_part_state PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_part_state PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_part_state PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_part_state PRIVATE /ignore:4099 /FORCE:MULTIPLE)

# Спрашивает по каждой партии и за ОДИН прогон, держит ли движок тот тембр, который
# держит прошивка, - по имени, чего измерение уровня не умеет. Собственный путь чтения
# из движка доказывает контролем "записал и прочитал обратно" до всех прочих показаний.
add_executable(d110_tone_clobber tone_clobber_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_tone_clobber PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_tone_clobber PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_tone_clobber PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_tone_clobber PRIVATE /ignore:4099 /FORCE:MULTIPLE)

# Различает две причины «Attempted to play unmapped key» в начале демо: карту ритма,
# которая ещё не доехала до движка, и карту, в которой клавиша действительно выключена.
add_executable(d110_rhythm_map rhythm_map_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_rhythm_map PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_rhythm_map PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_rhythm_map PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_rhythm_map PRIVATE /ignore:4099 /FORCE:MULTIPLE)

# Ищет путь, которым тип/время/уровень ревербератора доходят до микросхемы BOSS: жмёт
# кнопки, печатает экран и показывает рядом сдвиги ОЗУ и записи в регистр SO.
add_executable(d110_reverb_path reverb_path_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_reverb_path PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_reverb_path PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_reverb_path PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_reverb_path PRIVATE /ignore:4099 /FORCE:MULTIPLE)

# Карта регистров LA32: что прошивка кладёт в микросхему синтеза и какая ячейка что значит.
# Три раздражителя, отличающиеся ровно одним свойством, плюс окно без ноты как контроль.
add_executable(d110_la32_regmap la32_regmap_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_la32_regmap PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_la32_regmap PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_la32_regmap PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_la32_regmap PRIVATE /ignore:4099 /FORCE:MULTIPLE)

# Доезжает ли заводской банк патчей до самой прошивки и что он меняет в её памяти. Разницу
# «до и после» сравнивает с контрольным окном такой же длины БЕЗ импорта, потому что
# прошивка непрерывно пишет в свои рабочие области и сама по себе.
add_executable(d110_factory_bank factory_bank_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_factory_bank PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_factory_bank PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_factory_bank PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_factory_bank PRIVATE /ignore:4099 /FORCE:MULTIPLE)

# Ищет адрес, по которому прошивка задаёт ТИП ревербератора: перехватывает весь свободный
# внешний ввод-вывод 0x0400-0x0BFF, разворачивает восемь типов и сравнивает с контрольным
# прогоном, где нажатия и нота те же, а тип не меняется.
add_executable(d110_reverb_reg reverb_reg_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_reverb_reg PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_reverb_reg PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_reverb_reg PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_reverb_reg PRIVATE /ignore:4099 /FORCE:MULTIPLE)

# Переживает ли правка тембра смену параметра партии на ОБЫЧНОМ патче - вопрос, на который
# демо-песня ответить не может, потому что её группа тембра 5 с панели недостижима. Ставит
# опыт дважды в одном прогоне, с подтверждением тембров и без него, чтобы у результата был
# контроль, умеющий показать отказ.
add_executable(d110_tone_edit_survives tone_edit_survives_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_tone_edit_survives PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_tone_edit_survives PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_tone_edit_survives PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_tone_edit_survives PRIVATE /ignore:4099 /FORCE:MULTIPLE)

# Ищет момент, когда прошивка перепрограммирует микросхему ревербератора: пишет каждую
# запись в регистр SO вместе с адресом подпрограммы, которая её сделала.
add_executable(d110_so_trace so_trace_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_so_trace PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_so_trace PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_so_trace PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_so_trace PRIVATE /ignore:4099 /FORCE:MULTIPLE)

# Renders the LCD at several settings to PNG so the result can be looked at beside the
# reference photo. Calibration only - not part of the plugin's render path.
add_executable(d110_lcd_check lcd_render_check.cpp)
target_compile_definitions(d110_lcd_check PRIVATE
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_lcd_check PRIVATE juce::juce_gui_basics juce::juce_graphics)

# Checks that an imported SysEx bank actually reaches the control board, rather than only
# the sound engine - where the next RAM mirror silently undid it.
add_executable(d110_sysex_test sysex_test.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_sysex_test PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_sysex_test PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_sysex_test PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_sysex_test PRIVATE /ignore:4099 /FORCE:MULTIPLE)

# Reads the firmware back as MCS-96 assembly, using MAME's own disassembler. Needed to see
# what the loop hang_probe found is actually waiting for.
# D110Core.cpp comes along only for the emulator_info definitions MAME's frontend expects.
add_executable(d110_disasm disasm_tool.cpp Source/D110Core.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_disasm PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_disasm PRIVATE ${MAME_DEFS})
target_link_libraries(d110_disasm PRIVATE ${MAME_LIBS} ${MAME_SYS_LIBS})
target_link_options(d110_disasm PRIVATE /ignore:4099 /FORCE:MULTIPLE)

# Finds WHERE the firmware gets stuck once notes reach it, by sampling the program counter
# in a healthy state and a dead one and diffing the two histograms.
add_executable(d110_hang_probe hang_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_hang_probe PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_hang_probe PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_hang_probe PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_hang_probe PRIVATE /ignore:4099 /FORCE:MULTIPLE)

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
