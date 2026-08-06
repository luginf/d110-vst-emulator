cmake_minimum_required(VERSION 3.22)
# Номер версии - это оценка готовности в процентах, а не порядковый счётчик выпусков.
# 0.9.9 обещала девяносто девять и была слишком щедра: микросхема ревербератора BOSS не
# была эмулирована вовсе, а это заметная часть звука прибора, и восьми индивидуальных
# выходов тоже нет. Оценка честно опущена, хотя номер при этом идёт назад.
#
# 2026-08-04: оба исправлены. Ревербератор BOSS (чип HG61H20R36F/BOS-007, ПЗУ IC6)
# теперь звучит настоящей эмуляцией микрокода (BossEmu, перенесена из
# github.com/davidhsilaban/munt с сохранением авторства - Sergey V. Mikayev), а не
# приближением через четыре режима MT-32. Шесть индивидуальных выходов (MULTI OUT 1-6,
# плюс основная стерео-шина MIX) подключены как отдельные шины VST3, по умолчанию
# выключены - маршрутизация по Output Assign партии проверена измерением, звук приходит
# ровно в назначенный канал и ни в один другой.
project(D110Emulator VERSION 0.9.5)

# C++20 because MAME's headers need it.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# CRITICAL: on Windows, MAME's Release libs are built against the static runtime, so
# everything here - JUCE and mt32emu included - must use /MT too or the link mixes
# runtimes. Doesn't apply to the Linux (GCC) build.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

# Комментарии в этом проекте пишутся по-русски, а MSVC без этого ключа читает исходник
# в кодовой странице системы и на кириллице выдаёт C4819 - а в худшем случае съедает
# байт, похожий на обратную косую черту, в конце строки комментария.
if(MSVC)
    add_compile_options(/utf-8)
endif()

# OFF by default: D110EmulatorNative (the MAME-free native core, see below) is the primary
# supported build now that this fork keeps the original MAME-backed engine as a dormant
# fallback rather than the default. Turn this ON (and point mame.cmake's MAME_DIR at a built
# MAME 0.288+patch tree) to also build the original D110Emulator plugin and its MAME-only
# test probes.
option(D110_BUILD_MAME_BACKEND "Build the MAME-backed D110Emulator plugin and probes" OFF)

if(D110_BUILD_MAME_BACKEND)
    include("${CMAKE_CURRENT_SOURCE_DIR}/mame.cmake")
endif()

include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.15
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(JUCE)

# Build mt32emu as a static library, C++ interface only (matches rom_test setup).
# PIC is required so this static lib can be linked into the VST3 shared module on Linux.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(BUILD_SHARED_LIBS FALSE CACHE BOOL "" FORCE)
set(libmt32emu_SHARED FALSE CACHE BOOL "" FORCE)
set(libmt32emu_C_INTERFACE FALSE CACHE BOOL "" FORCE)
set(libmt32emu_CPP_INTERFACE TRUE CACHE BOOL "" FORCE)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../munt/mt32emu" "${CMAKE_CURRENT_BINARY_DIR}/mt32emu_build")

# The front panel IS the reference photograph - see docs/panel_reference_notes.md.
juce_add_binary_data(D110PanelData
    SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../docs/panel_reference.png"
            "${CMAKE_CURRENT_SOURCE_DIR}/../docs/memory_card_m256d.png"
            "${CMAKE_CURRENT_SOURCE_DIR}/../docs/D20infos.png")

# On Windows, plugins conventionally live in the shared Program Files VST3 folder.
# Elsewhere, JUCE's own default (~/Library/Audio/Plug-Ins/VST3 on macOS, ~/.vst3 on Linux)
# is already the right per-user location, so leave VST3_COPY_DIR unset there.
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(D110_VST3_COPY_DIR_ARGS VST3_COPY_DIR "C:/Program Files/Common Files/VST3")
else()
    set(D110_VST3_COPY_DIR_ARGS)
endif()

# AU (Audio Unit) only exists on Apple platforms - JUCE errors out if asked to build it
# elsewhere, so this has to be conditional rather than just always appended.
set(D110_FORMATS VST3 Standalone)
if(APPLE)
    list(APPEND D110_FORMATS AU)
endif()

if(D110_BUILD_MAME_BACKEND)

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
    ${D110_VST3_COPY_DIR_ARGS}
    PLUGIN_MANUFACTURER_CODE Manu
    PLUGIN_CODE Dvst
    FORMATS ${D110_FORMATS}
    PRODUCT_NAME "D-110_Emulator"
    PLUGIN_NAME "D-110 Emulator"
)

# On Apple, MAME_DRIVLIST is built as its own tiny static lib instead of going straight into
# D110Emulator's sources, so the foreach() below can -force_load JUST that (see its comment
# for why: force-loading the whole D110Emulator archive pulled in D110Core.cpp.o's
# emulator_info:: overrides a second time, which are already reached normally, and ld64 has
# no --allow-multiple-definition to paper over the resulting duplicate-symbol error the way
# the Windows/Linux branches do).
if(APPLE)
    add_library(D110DriverList STATIC "${MAME_DRIVLIST}")
    target_include_directories(D110DriverList PRIVATE ${MAME_INCLUDES})
    target_compile_definitions(D110DriverList PRIVATE ${MAME_DEFS})
    set(D110_DRIVLIST_SOURCE)
else()
    set(D110_DRIVLIST_SOURCE "${MAME_DRIVLIST}")
endif()

target_sources(D110Emulator PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    ${D110_DRIVLIST_SOURCE}
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

if(APPLE)
    target_link_libraries(D110Emulator PRIVATE D110DriverList)
endif()

# MAME's own emulator_info definitions collide with ours by design, and its libs are
# built with a different debug-info setting than JUCE's.
foreach(_fmt VST3 Standalone AU)
    if(TARGET D110Emulator_${_fmt})
        target_link_options(D110Emulator_${_fmt} PRIVATE ${MAME_LINK_OPTIONS})
        if(APPLE)
            # Same problem as the Linux branch below (a static-archive member only pulled in
            # if something already needs it at that point in the link, and nothing does until
            # the MAME libs reference it later), ld64's answer instead of GNU ld's
            # --whole-archive/--no-whole-archive: force_load takes one archive, not a region,
            # so it needs its own flag per invocation rather than a pair of markers.
            # Confirmed by CI: force_load-ing the WHOLE D110Emulator archive (first attempt)
            # duplicate-symbol'd D110Core.cpp.o's emulator_info:: overrides against
            # libfrontend.a's mame.o, because D110Core.cpp.o is already reached normally and
            # force_load pulled it in a second time. D110DriverList is a single-object archive
            # (see above) holding only the generated driver_list table, so forcing it can't
            # cause that - it has nothing else in it to duplicate.
            target_link_options(D110Emulator_${_fmt} PRIVATE
                -Wl,-force_load,$<TARGET_FILE:D110DriverList>)
        elseif(NOT MSVC)
            # GNU ld only extracts a .a member when something already needs it at that point
            # in the link. libD-110_Emulator_SharedCode.a (which the MAME_DRIVLIST object -
            # driver_list::s_driver_count/s_drivers_sorted - ends up archived into) is linked
            # before the MAME libs that reference those symbols, so it's never pulled in.
            # --whole-archive forces every member in, same as a plain .o would behave.
            target_link_options(D110Emulator_${_fmt} PRIVATE
                -Wl,--whole-archive $<TARGET_FILE:D110Emulator> -Wl,--no-whole-archive)
        endif()
    endif()
endforeach()

target_link_libraries(D110Emulator PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

endif() # D110_BUILD_MAME_BACKEND

# ---------------------------------------------------------------------------
# D110EmulatorNative: the SAME PluginProcessor/PluginEditor, compiled with D110_NATIVE_CORE
# so D110CoreType resolves to D110CoreNative instead of the MAME-backed D110Core - see the
# "Native D-110 CPU core port" plan and PluginProcessor.h's own comment on D110CoreType.
# Deliberately links none of MAME_LIBS/MAME_INCLUDES/D110Core.cpp/MAME_DRIVLIST - that
# absence is the whole point of this target, not an optimisation. A separate plugin (own
# product name, own plugin code, own install dir) so it can sit beside the real, proven
# D110Emulator in a DAW's plugin list rather than replacing it, exactly as the plan's
# "Backend switch design" calls for: the MAME-backed plugin stays the shipping default
# until this one has passed the full probe suite pairwise against it.
juce_add_plugin(D110EmulatorNative
    VERSION ${PROJECT_VERSION}
    COMPANY_NAME "DIY"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD TRUE
    ${D110_VST3_COPY_DIR_ARGS}
    PLUGIN_MANUFACTURER_CODE Manu
    PLUGIN_CODE Dvsn
    FORMATS ${D110_FORMATS}
    # Not "D-110 Emulator (Native)": parentheses in the product name end up in the .vst3
    # bundle's own path, and CMake's Unix Makefiles generator doesn't shell-escape them in
    # the generated copy/remove recipes - `(` reaches /bin/sh unquoted and breaks the build.
    # Spaces are escaped there; parens are not, on Linux. Windows never hit this.
    PRODUCT_NAME "D-110 Emulator Native"
)

target_sources(D110EmulatorNative PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/native/mcs96_cpu.cpp
    Source/native/d110_bus.cpp
    Source/native/msm6222b.cpp
    Source/native/D110CoreNative.cpp
    Source/native/ram_mirror.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
)

target_compile_definitions(D110EmulatorNative PRIVATE D110_NATIVE_CORE=1)

target_compile_definitions(D110EmulatorNative PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
)

target_link_libraries(D110EmulatorNative PRIVATE
    D110PanelData
    MT32Emu::mt32emu
    juce::juce_audio_utils
    juce::juce_audio_processors
)

target_link_libraries(D110EmulatorNative PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

if(D110_BUILD_MAME_BACKEND)

# ---------------------------------------------------------------------------
# Offline audio check: instantiates the real processor, powers it on, plays a note
# and measures the output. No DAW, no audio device. Also the harness for proving
# that a panel edit becomes audible once the RAM bridge is in.
# ---------------------------------------------------------------------------
add_executable(d110_audio_test audio_test.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_audio_test PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_audio_test PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_audio_test PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_audio_test PRIVATE ${MAME_LINK_OPTIONS})

# Maps the firmware's battery RAM onto Roland's SysEx addresses by experiment: presses
# panel buttons and reports which bytes moved. See docs/sysex_address_map.md.
add_executable(d110_bridge_probe bridge_probe.cpp Source/D110Core.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_bridge_probe PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_bridge_probe PRIVATE ${MAME_DEFS})
target_link_libraries(d110_bridge_probe PRIVATE ${MAME_LIBS} ${MAME_SYS_LIBS})
target_link_options(d110_bridge_probe PRIVATE ${MAME_LINK_OPTIONS})

# ---------------------------------------------------------------------------
# Locates the Tone Temporary Area in the firmware's RAM by comparing bytes: reads the
# tone mt32emu holds for each part back out of the engine and slides it over the whole
# 32 KB. An exact match both pins the base and proves that mirroring an unedited tone
# changes nothing - the null test the bridge needed before the deep Edit pages could be
# wired up. See docs/sysex_address_map.md.
# ---------------------------------------------------------------------------
add_executable(d110_tone_probe tone_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_tone_probe PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_tone_probe PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_tone_probe PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_tone_probe PRIVATE ${MAME_LINK_OPTIONS})

# Checks that loading the plugin does NOT re-initialise the instrument, and that the
# hardware's own factory init (latch WRITE/COPY, power on, ENTER) works from the panel.
add_executable(d110_coldstart_test coldstart_test.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_coldstart_test PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_coldstart_test PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_coldstart_test PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_coldstart_test PRIVATE ${MAME_LINK_OPTIONS})

# Checks that a project recalls its own firmware memory, that two instances get separate
# memory, and that the second one refuses to power on instead of crashing the host.
add_executable(d110_state_test state_test.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_state_test PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_state_test PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_state_test PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_state_test PRIVATE ${MAME_LINK_OPTIONS})

# Discovers the LA32's register interface by logging every access to an address the D-110's
# map leaves unclaimed - which is precisely where the missing sound board would answer.
add_executable(d110_la32_probe la32_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_la32_probe PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_la32_probe PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_la32_probe PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_la32_probe PRIVATE ${MAME_LINK_OPTIONS})

# One-shot: play a single note, freeze it (no interrupt answered), dump the raw voice
# bookkeeping tables - ground truth for what StuckPolicy::La32Stub's slot scan should see.
add_executable(d110_la32_lifecycle la32_lifecycle_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_la32_lifecycle PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_la32_lifecycle PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_la32_lifecycle PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_la32_lifecycle PRIVATE ${MAME_LINK_OPTIONS})

# Live write tap over the voice-context bookkeeping arrays (f3c0..f480), engaged
# together with StuckPolicy::La32Stub - answers whether the firmware's own housekeeping
# ever reaches f460[] bit 6 at all (the second stall) instead of guessing from statics.
add_executable(d110_la32_ctx la32_ctx_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_la32_ctx PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_la32_ctx PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_la32_ctx PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_la32_ctx PRIVATE ${MAME_LINK_OPTIONS})

# Practical question hang_probe/longrun_test don't ask: with La32Stub engaged, does the
# panel survive NORMAL playing (single notes, realistic gaps), not deliberate chord-stress.
add_executable(d110_la32_realistic la32_realistic_test.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_la32_realistic PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_la32_realistic PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_la32_realistic PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_la32_realistic PRIVATE ${MAME_LINK_OPTIONS})

# Reproduces the user's own reported steps directly - EDIT+ENTER into ROM Play, ENTER to
# play the demo song - instead of only inferring from synthetic MIDI tests that it hits
# the same freeze. Also checks whether La32Stub fares differently here, since the demo
# song runs entirely on MAME's own thread with no host MIDI injection at all.
add_executable(d110_demo_song_repro demo_song_repro.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_demo_song_repro PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_demo_song_repro PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_demo_song_repro PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_demo_song_repro PRIVATE ${MAME_LINK_OPTIONS})

# Does a factory reset actually restore the documented per-part pan fan, and does that
# byte reach the sound engine as a real stereo shift? Answers the "panning feels wrong /
# it won't return to defaults" question by measurement rather than by listening.
add_executable(d110_pan_verify pan_verify.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_pan_verify PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_pan_verify PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_pan_verify PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_pan_verify PRIVATE ${MAME_LINK_OPTIONS})

# The demo song is silent because the firmware plays it internally and never transmits it.
# This checks whether the notes are nevertheless recoverable from the firmware's own RAM
# (f400 = note, f420 = velocity, f3a0 = part?), which would let the bridge carry them.
add_executable(d110_note_source note_source_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_note_source PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_note_source PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_note_source PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_note_source PRIVATE ${MAME_LINK_OPTIONS})

# Renders the demo song to a WAV so "is there any sound at all" is answered by a file the
# user can listen to, not by inference.
add_executable(d110_demo_wav demo_wav_render.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_demo_wav PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_demo_wav PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_demo_wav PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_demo_wav PRIVATE ${MAME_LINK_OPTIONS})

# Sounds each part on its own and measures it, so "part 6 lights up but makes no noise" is
# answered per part instead of by ear across nine at once. Two passes: straight into the
# engine, and through the firmware on that part's own channel.
add_executable(d110_part_audio part_audio_test.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_part_audio PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_part_audio PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_part_audio PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_part_audio PRIVATE ${MAME_LINK_OPTIONS})

# Renders the demo song once per part, with only that part delivered, and measures what
# comes out. Note counts cannot tell "silent" from "buried"; a level can.
add_executable(d110_solo_level solo_level_test.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_solo_level PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_solo_level PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_solo_level PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_solo_level PRIVATE ${MAME_LINK_OPTIONS})

# Compares, per part and DURING the demo, what the firmware holds against what the engine
# holds. A level 20 dB adrift is either the bridge failing to carry something or the
# arrangement; only a byte-for-byte comparison tells them apart.
add_executable(d110_part_state part_state_compare.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_part_state PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_part_state PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_part_state PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_part_state PRIVATE ${MAME_LINK_OPTIONS})

# Спрашивает по каждой партии и за ОДИН прогон, держит ли движок тот тембр, который
# держит прошивка, - по имени, чего измерение уровня не умеет. Собственный путь чтения
# из движка доказывает контролем "записал и прочитал обратно" до всех прочих показаний.
add_executable(d110_tone_clobber tone_clobber_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_tone_clobber PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_tone_clobber PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_tone_clobber PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_tone_clobber PRIVATE ${MAME_LINK_OPTIONS})

# Различает две причины «Attempted to play unmapped key» в начале демо: карту ритма,
# которая ещё не доехала до движка, и карту, в которой клавиша действительно выключена.
add_executable(d110_rhythm_map rhythm_map_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_rhythm_map PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_rhythm_map PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_rhythm_map PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_rhythm_map PRIVATE ${MAME_LINK_OPTIONS})

# Ищет путь, которым тип/время/уровень ревербератора доходят до микросхемы BOSS: жмёт
# кнопки, печатает экран и показывает рядом сдвиги ОЗУ и записи в регистр SO.
add_executable(d110_reverb_path reverb_path_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_reverb_path PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_reverb_path PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_reverb_path PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_reverb_path PRIVATE ${MAME_LINK_OPTIONS})

# Карта регистров LA32: что прошивка кладёт в микросхему синтеза и какая ячейка что значит.
# Три раздражителя, отличающиеся ровно одним свойством, плюс окно без ноты как контроль.
add_executable(d110_la32_regmap la32_regmap_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_la32_regmap PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_la32_regmap PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_la32_regmap PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_la32_regmap PRIVATE ${MAME_LINK_OPTIONS})

# Доезжает ли заводской банк патчей до самой прошивки и что он меняет в её памяти. Разницу
# «до и после» сравнивает с контрольным окном такой же длины БЕЗ импорта, потому что
# прошивка непрерывно пишет в свои рабочие области и сама по себе.
add_executable(d110_factory_bank factory_bank_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_factory_bank PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_factory_bank PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_factory_bank PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_factory_bank PRIVATE ${MAME_LINK_OPTIONS})

# Обратный путь моста: доходит ли правка, посланная эксклюзивным сообщением, до самой
# прошивки, куда она ложится и не запрещает ли её Mem Protect. Это фундамент расширенного
# редактора - без него правка из редактора была бы догадкой. Заодно измеряет место памяти
# ТОНОВ, которого не найти по содержимому: в заводском приборе она пуста.
add_executable(d110_editor_write editor_write_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_editor_write PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_editor_write PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_editor_write PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_editor_write PRIVATE ${MAME_LINK_OPTIONS})

# Снимок расширенного редактора в PNG - настоящего компонента, с живыми значениями из
# работающего прибора. Судить о разметке по константам нельзя, поэтому её снимают и смотрят.
add_executable(d110_editor_shot editor_shot.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_editor_shot PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_editor_shot PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_editor_shot PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_editor_shot PRIVATE ${MAME_LINK_OPTIONS})

# Доходит ли правка из расширенного редактора до прибора, до звука и обратно. Проверяет
# каждый отправитель по памяти прошивки, слышимость - по RMS с контрольным прогоном, и
# переход на патч - по номеру, на котором прибор в итоге стоит.
#
# JUCE_MODAL_LOOPS_PERMITTED здесь включён намеренно и только для этой цели: переход на патч
# идёт по таймеру очереди сообщений, а в консольной программе эту очередь надо крутить руками.
add_executable(d110_editor_test editor_test.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_editor_test PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_editor_test PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(d110_editor_test PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_editor_test PRIVATE ${MAME_LINK_OPTIONS})

# Ищет адрес, по которому прошивка задаёт ТИП ревербератора: перехватывает весь свободный
# внешний ввод-вывод 0x0400-0x0BFF, разворачивает восемь типов и сравнивает с контрольным
# прогоном, где нажатия и нота те же, а тип не меняется.
add_executable(d110_reverb_reg reverb_reg_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_reverb_reg PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_reverb_reg PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_reverb_reg PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_reverb_reg PRIVATE ${MAME_LINK_OPTIONS})

# Переживает ли правка тембра смену параметра партии на ОБЫЧНОМ патче - вопрос, на который
# демо-песня ответить не может, потому что её группа тембра 5 с панели недостижима. Ставит
# опыт дважды в одном прогоне, с подтверждением тембров и без него, чтобы у результата был
# контроль, умеющий показать отказ.
add_executable(d110_tone_edit_survives tone_edit_survives_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_tone_edit_survives PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_tone_edit_survives PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_tone_edit_survives PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_tone_edit_survives PRIVATE ${MAME_LINK_OPTIONS})

# Первый мост от регистров к звуку: снятое с прошивки состояние прогоняется через модель той
# же микросхемы из munt. Включает её внутренние заголовки - библиотека статическая, символы на
# месте, а публичного интерфейса у генератора волны нет и не задумано.
add_executable(d110_la32_render la32_render.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_la32_render PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../munt/mt32emu/src"
    "${CMAKE_CURRENT_BINARY_DIR}/mt32emu_build/src")
target_compile_definitions(d110_la32_render PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_la32_render PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_la32_render PRIVATE ${MAME_LINK_OPTIONS})

# Что банк 0x0CC0 несёт во времени, пока нота звучит. Печатает поток как ряд, а не сравнивает
# множества значений - на этом сравнении три прежних захода на этот банк и провалились.
add_executable(d110_la32_stream la32_stream_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_la32_stream PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_la32_stream PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_la32_stream PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_la32_stream PRIVATE ${MAME_LINK_OPTIONS})

# Снимает настоящую панель в PNG, чтобы её было на что посмотреть рядом с фотографией.
add_executable(d110_panel_render panel_render.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_panel_render PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
# Ход карты двигает собственный таймер панели, поэтому стенду нужен настоящий цикл
# сообщений; в плагине он не нужен и не включается.
target_compile_definitions(d110_panel_render PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(d110_panel_render PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_panel_render PRIVATE ${MAME_LINK_OPTIONS})

# Чем прошивка отличает вставленную карту памяти от пустого гнезда. Подставляет ей четыре
# разные карты и снимает с экрана, что она о каждой сказала.
add_executable(d110_card card_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_card PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_card PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_card PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_card PRIVATE ${MAME_LINK_OPTIONS})

# Ищет момент, когда прошивка перепрограммирует микросхему ревербератора: пишет каждую
# запись в регистр SO вместе с адресом подпрограммы, которая её сделала.
add_executable(d110_so_trace so_trace_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_so_trace PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_so_trace PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_so_trace PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_so_trace PRIVATE ${MAME_LINK_OPTIONS})

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
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_sysex_test PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_sysex_test PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_sysex_test PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_sysex_test PRIVATE ${MAME_LINK_OPTIONS})

# Reads the firmware back as MCS-96 assembly, using MAME's own disassembler. Needed to see
# what the loop hang_probe found is actually waiting for.
# D110Core.cpp comes along only for the emulator_info definitions MAME's frontend expects.
add_executable(d110_disasm disasm_tool.cpp Source/D110Core.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_disasm PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_disasm PRIVATE ${MAME_DEFS})
target_link_libraries(d110_disasm PRIVATE ${MAME_LIBS} ${MAME_SYS_LIBS})
target_link_options(d110_disasm PRIVATE ${MAME_LINK_OPTIONS})

# Finds WHERE the firmware gets stuck once notes reach it, by sampling the program counter
# in a healthy state and a dead one and diffing the two histograms.
add_executable(d110_hang_probe hang_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_hang_probe PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_hang_probe PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_hang_probe PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_hang_probe PRIVATE ${MAME_LINK_OPTIONS})

# Isolates why the control board stops responding after some seconds of playing: runs the
# firmware for a minute with and without host MIDI, pressing a button throughout.
add_executable(d110_longrun_test longrun_test.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_longrun_test PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_longrun_test PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_longrun_test PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_longrun_test PRIVATE ${MAME_LINK_OPTIONS})

# Records what really happens when two machines run in one process, which is what a DAW
# does as soon as a second instance of the plugin is loaded. Diagnostic, not a fix.
add_executable(d110_two_instance_test two_instance_test.cpp Source/D110Core.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_two_instance_test PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_two_instance_test PRIVATE ${MAME_DEFS})
target_link_libraries(d110_two_instance_test PRIVATE ${MAME_LIBS} ${MAME_SYS_LIBS})
target_link_options(d110_two_instance_test PRIVATE ${MAME_LINK_OPTIONS})

# Console check of the D110Core lifecycle: boots the real firmware in-process and prints
# the MSM6222B's own dot matrix while pressing front-panel buttons. Proves the wrap
# independently of the plugin.
add_executable(d110_core_test core_test.cpp Source/D110Core.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_core_test PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_core_test PRIVATE ${MAME_DEFS})
target_link_libraries(d110_core_test PRIVATE ${MAME_LIBS} ${MAME_SYS_LIBS})
target_link_options(d110_core_test PRIVATE ${MAME_LINK_OPTIONS})

# Куда деваются ноты при быстрой игре: их теряет заглушка LA32 или их некуда играть.
# Считает ноту на трёх рубежах - отправлено, взято прошивкой, занято у движка - и делает это
# дважды, тоном в два партиала и тоном в четыре, чтобы полифонию нельзя было спутать с потерей.
add_executable(d110_polyphony polyphony_test.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_polyphony PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_polyphony PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_polyphony PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_polyphony PRIVATE ${MAME_LINK_OPTIONS})

# Two parts (bass + solo) playing fast, overlapping lines AT THE SAME TIME - the user's actual
# reported shape (both parts losing notes together), not one part tested in isolation.
add_executable(d110_multi_part_polyphony multi_part_polyphony_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_multi_part_polyphony PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_multi_part_polyphony PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_multi_part_polyphony PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_multi_part_polyphony PRIVATE ${MAME_LINK_OPTIONS})

# Measures the real DSP cost of raising mt32emu's usePartialCount - no real-time pacing in its
# timed section, unlike every other probe here, on purpose.
add_executable(d110_partial_count_cpu partial_count_cpu_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_partial_count_cpu PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_partial_count_cpu PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_partial_count_cpu PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_partial_count_cpu PRIVATE ${MAME_LINK_OPTIONS})

# Owner's live-DAW report: D110Emulator (MAME-backed) sustains notes forever and freezes the
# LCD on Fantasy/Steel Drum, while D110EmulatorNative is fine - so this is backend-specific,
# not the mechanism fixed earlier this session (which was ported to both).
add_executable(d110_mame_stuck_note_repro mame_stuck_note_repro.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_mame_stuck_note_repro PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_mame_stuck_note_repro PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_mame_stuck_note_repro PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_mame_stuck_note_repro PRIVATE ${MAME_LINK_OPTIONS})

# Кто пишет в таблицу слотов LA32 и освобождает ли её хоть кто-нибудь. Перехват записей в
# окно диспетчеризации даёт адрес, значение и PC - то есть отвечает не догадкой, а именем
# подпрограммы. См. docs/la32_interface.md.
add_executable(d110_slot_life slot_life_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_slot_life PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_slot_life PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_slot_life PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_slot_life PRIVATE ${MAME_LINK_OPTIONS})

# Восстановление настоящей батарейной памяти прибора после порчи файла (например, резким
# обрывом питания посреди записи). Идёт тем же путём, что и сам плагин, - через
# D110AudioProcessor::getNvramRoot() без единого своего аргумента, - поэтому правит именно
# тот файл, который откроет установленный VST3.
add_executable(d110_nvram_recovery nvram_recovery.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_nvram_recovery PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_nvram_recovery PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_nvram_recovery PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_nvram_recovery PRIVATE ${MAME_LINK_OPTIONS})

# Проверяет документированную процедуру факт-сброса ("POWER off, Ctrl+click WRITE/COPY,
# POWER on, ENTER") настоящими нажатиями по настоящей D110Panel - в отличие от
# coldstart_test.cpp, который вызывает D110Core::setButton() в обход панели и потому не мог
# бы поймать регрессию в самой D110Panel::setButtonState().
add_executable(d110_panel_factory_init panel_factory_init_test.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_panel_factory_init PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_panel_factory_init PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_panel_factory_init PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_panel_factory_init PRIVATE ${MAME_LINK_OPTIONS})

# Насколько на самом деле расходится время регистрации ноты прошивкой, если несколько
# партий бьют одновременно - измеряет по-настоящему через D110Core::startNoteLog(), а не
# пересчитывает байтовую скорость на бумаге. См. docs про общую очередь MIDI к прошивке.
add_executable(d110_midi_congestion midi_congestion_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_midi_congestion PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_midi_congestion PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_midi_congestion PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_midi_congestion PRIVATE ${MAME_LINK_OPTIONS})

# Заливка банка тонов в настоящую память прибора. Отдельный инструмент, а не часть
# nvram_recovery: тот возвращает заводское состояние, а память тонов заводским состоянием
# ПУСТА, и наполнить её можно только банком со стороны.
add_executable(d110_bank_import bank_import.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_bank_import PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_bank_import PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_bank_import PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_bank_import PRIVATE ${MAME_LINK_OPTIONS})

# Воспроизведение жалобы «ноты залипают на всех партиях сразу». Отдельно от d110_polyphony:
# тот меряет ПОТЕРИ нот на одной партии, а этот - ОСТАТОК при отпущенных клавишах на всех
# девяти, и это противоположные признаки.
add_executable(d110_stuck_notes stuck_notes_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_stuck_notes PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_stuck_notes PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_stuck_notes PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_stuck_notes PRIVATE ${MAME_LINK_OPTIONS})

# MAME-backed counterpart to d110_native_ramp_edge_stress - same dense overlapping/legato
# pattern, against D110Core.cpp's own midiTick() fix.
add_executable(d110_mame_ramp_edge_stress mame_ramp_edge_stress_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_mame_ramp_edge_stress PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_mame_ramp_edge_stress PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_mame_ramp_edge_stress PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_mame_ramp_edge_stress PRIVATE ${MAME_LINK_OPTIONS})

# Играет каждую ритм-клавишу по отдельности и меряет пик. Отвечает на "closed hat не
# звучит" измерением, а не догадкой - список молчащих клавиш даёт прямо, без предположений
# о том, какая именно сломана.
add_executable(d110_rhythm_key rhythm_key_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_rhythm_key PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_rhythm_key PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_rhythm_key PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_rhythm_key PRIVATE ${MAME_LINK_OPTIONS})

# Проверка: возвращает ли повторный выбор текущего патча живую Timbre Temp к данным патча.
# Живая область читалась сплошным центром (pan 7 у всех восьми) при верных данных в самом
# патче - вопрос в том, чинится ли это тем же нажатием, каким пользуется панель.
add_executable(d110_pan_resync pan_resync_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_pan_resync PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_pan_resync PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(d110_pan_resync PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_pan_resync PRIVATE ${MAME_LINK_OPTIONS})

# Проверка на девственной, временной НВР: сразу после заводского сброса, ДО единого нажатия
# пользователем, читается ли живая панорама верно или сплошным центром - разводит "баг
# загрузки прошивки" и "накопленный след сегодняшних опытов в реальном файле".
add_executable(d110_virgin_boot virgin_boot_probe.cpp Source/D110Core.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_virgin_boot PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_virgin_boot PRIVATE ${MAME_DEFS})
target_link_libraries(d110_virgin_boot PRIVATE ${MAME_LIBS} ${MAME_SYS_LIBS})
target_link_options(d110_virgin_boot PRIVATE ${MAME_LINK_OPTIONS})

# Трассирует запись в таблицы полного круга голоса (dispatch + completion) для одного удара
# по ритм-клавише, чтобы найти источник "Attempted to play invalid key 1" рядом с closed
# hat/open hat: пишет ли это сама прошивка в f400[], или подмена происходит в нашем мосте.
add_executable(d110_rhythm_ctx rhythm_ctx_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_rhythm_ctx PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_rhythm_ctx PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_rhythm_ctx PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_rhythm_ctx PRIVATE ${MAME_LINK_OPTIONS})

# «Патчи в демо-песне не соответствуют» - разводит два возможных объяснения: демо трогает
# Patch/Timbre Memory (значит зависит от того, что реально лежит в батарейной памяти
# пользователя, у которой нет настоящих заводских имён без карты FACTORY PRESET), или демо
# полностью самодостаточна и патчей пользователя не касается вовсе.
add_executable(d110_demo_patch demo_patch_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_demo_patch PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_demo_patch PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(d110_demo_patch PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_demo_patch PRIVATE ${MAME_LINK_OPTIONS})

# «Синхронизация с Ableton плывёт» - меряет реальную задержку между подачей ноты в
# processBlock и моментом, когда прошивка сама её регистрирует (NoteLog). Демо-песня этого
# круга не проходит вовсе (прошивка играет сама себе), поэтому и не плывёт.
add_executable(d110_note_latency note_latency_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_note_latency PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_note_latency PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_note_latency PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_note_latency PRIVATE ${MAME_LINK_OPTIONS})

# Проверяет перенесённый BOSS-ревербератор не на компиляции, а на звуке: грузится ли
# ic6.bin, доходит ли байт типа ревербератора (System 0x2D95) до BossEmu, и дают ли разные
# из восьми типов заметно разный хвост затухания.
add_executable(d110_boss_reverb boss_reverb_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_boss_reverb PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_boss_reverb PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(d110_boss_reverb PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_boss_reverb PRIVATE ${MAME_LINK_OPTIONS})

# Проверка одного типа ревербератора в СВЕЖЕМ экземпляре (аргумент argv[1]) - разводит
# "тип сам по себе тих" от "переключение между типами что-то ломает".
add_executable(d110_boss_reverb_iso boss_reverb_isolated_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_boss_reverb_iso PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_boss_reverb_iso PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(d110_boss_reverb_iso PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_boss_reverb_iso PRIVATE ${MAME_LINK_OPTIONS})

# Проверяет перенесённые шесть индивидуальных выходов на маршрутизации звука, не только на
# компиляции: включает все доп. шины, меняет Output Assign партии 1, меряет энергию в
# каждом из восьми каналов.
add_executable(d110_multi_output multi_output_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_multi_output PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_multi_output PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(d110_multi_output PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_multi_output PRIVATE ${MAME_LINK_OPTIONS})

# Регрессия по слуху: играешь кик (35), потом Closed Hi-Hat (42) - и на хай-хэте звучит
# соседний барабан. Причина была в том, что подсказка для подмены ставилась в очередь для
# ЛЮБОЙ ритм-ноты, а не только для трёх сломанных клавиш - рабочие клавиши душили очередь.
add_executable(d110_rhythm_hint_bleed rhythm_hint_bleed_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_rhythm_hint_bleed PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_rhythm_hint_bleed PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(d110_rhythm_hint_bleed PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_rhythm_hint_bleed PRIVATE ${MAME_LINK_OPTIONS})

# Диагностика: pan_verify.cpp только что показал 6 из 8 партий беззвучными после заводского
# сброса. Дамп живой Timbre Temp/Tone Temp для всех восьми партий сразу после того же
# сброса - разделяет "правки Synth.cpp сломали рендер" и "живая область просто не
# заполняется сбросом для большинства партий".
add_executable(d110_pan_verify_diag pan_verify_diag_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_pan_verify_diag PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_pan_verify_diag PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(d110_pan_verify_diag PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_pan_verify_diag PRIVATE ${MAME_LINK_OPTIONS})

# Настоящее звуковое измерение панорамы барабанов - до сих пор сверяли только байт в
# памяти против заводского дампа, никогда реальный звук по обоим каналам.
add_executable(d110_rhythm_pan_audio rhythm_pan_audio_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_rhythm_pan_audio PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_rhythm_pan_audio PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(d110_rhythm_pan_audio PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_rhythm_pan_audio PRIVATE ${MAME_LINK_OPTIONS})

endif() # D110_BUILD_MAME_BACKEND

# ---------------------------------------------------------------------------
# Native D-110 CPU core port (see the "Native D-110 CPU core port" plan) - the D-110's own
# i8x9x/MCS-96 CPU, hand-ported out of MAME's device/scheduler framework into a standalone
# interpreter (Source/native/), the same treatment DX100-VST already gave its DX100. Runs
# stepped inline on the audio thread instead of on MAME's own real-time thread, which is
# what the MAME-backed D110Core needs and is the root cause of its measured MIDI-to-sound
# jitter (note_latency_probe.cpp). Phase 1 scope only: CPU core + a minimal memory map
# (bank register, ROM windows, rams window; LCD/panel/SO stubbed) - enough to boot the real
# firmware ROM and prove the interpreter itself is correct via a PC-trace comparison against
# the MAME-backed core. Deliberately links NONE of MAME_LIBS/MAME_INCLUDES/JUCE - that
# absence is itself the thing this phase has to prove, not just an optimization.
# ---------------------------------------------------------------------------
add_executable(d110_native_boot native_boot_probe.cpp
    Source/native/mcs96_cpu.cpp Source/native/d110_bus.cpp Source/native/msm6222b.cpp)
target_include_directories(d110_native_boot PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# Phase 2 gate: LCD + panel buttons, verified by the same ASCII-art dump technique as
# core_test.cpp (the MAME-backed original) - run both, compare the printed screens by eye.
# Still zero MAME/JUCE dependency, same as d110_native_boot above.
add_executable(d110_native_core_test native_core_test.cpp
    Source/native/mcs96_cpu.cpp Source/native/d110_bus.cpp Source/native/msm6222b.cpp
    Source/native/D110CoreNative.cpp Source/native/ram_mirror.cpp)
target_include_directories(d110_native_core_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# Phase 3 gate, NVRAM half: round-trip through D110CoreNative::start()/stop(), and format
# compatibility against a folder an existing MAME-backed D110Core session already wrote.
add_executable(d110_native_nvram native_nvram_probe.cpp
    Source/native/mcs96_cpu.cpp Source/native/d110_bus.cpp Source/native/msm6222b.cpp
    Source/native/D110CoreNative.cpp Source/native/ram_mirror.cpp)
target_include_directories(d110_native_nvram PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# Phase 3 gate, RAM-mirror half: does popSysex() actually hand back well-formed DT1 messages
# once the native core is past its boot-settle resync.
add_executable(d110_native_mirror native_mirror_probe.cpp
    Source/native/mcs96_cpu.cpp Source/native/d110_bus.cpp Source/native/msm6222b.cpp
    Source/native/D110CoreNative.cpp Source/native/ram_mirror.cpp)
target_include_directories(d110_native_mirror PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# Phase 4 gate: MIDI-in -> firmware -> NoteEvent round trip.
add_executable(d110_native_note native_note_probe.cpp
    Source/native/mcs96_cpu.cpp Source/native/d110_bus.cpp Source/native/msm6222b.cpp
    Source/native/D110CoreNative.cpp Source/native/ram_mirror.cpp)
target_include_directories(d110_native_note PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# Chord stress test - exercises serviceLa32Stub()'s per-context slot lookup, which a single
# note never needs to distinguish.
add_executable(d110_native_chord native_chord_probe.cpp
    Source/native/mcs96_cpu.cpp Source/native/d110_bus.cpp Source/native/msm6222b.cpp
    Source/native/D110CoreNative.cpp Source/native/ram_mirror.cpp)
target_include_directories(d110_native_chord PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# First real sound: native core + real mt32emu Synth, rendered to a WAV file. Links MT32Emu
# only - still no MAME, no JUCE.
add_executable(d110_native_audio native_audio_probe.cpp
    Source/native/mcs96_cpu.cpp Source/native/d110_bus.cpp Source/native/msm6222b.cpp
    Source/native/D110CoreNative.cpp Source/native/ram_mirror.cpp)
target_include_directories(d110_native_audio PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(d110_native_audio PRIVATE MT32Emu::mt32emu)

# Phase 6 gate: factory reset done entirely through the native core, checked against the
# same known factory-default bytes the MAME-backed tool's output was checked against by hand.
add_executable(d110_native_factory_reset native_factory_reset_probe.cpp
    Source/native/mcs96_cpu.cpp Source/native/d110_bus.cpp Source/native/msm6222b.cpp
    Source/native/D110CoreNative.cpp Source/native/ram_mirror.cpp)
target_include_directories(d110_native_factory_reset PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# The actual jitter measurement, same technique as note_latency_probe.cpp against the
# MAME-backed core - see that file's own header for why this is the real question.
add_executable(d110_native_note_latency native_note_latency_probe.cpp
    Source/native/mcs96_cpu.cpp Source/native/d110_bus.cpp Source/native/msm6222b.cpp
    Source/native/D110CoreNative.cpp Source/native/ram_mirror.cpp)
target_include_directories(d110_native_note_latency PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# The check that actually matters: the REAL D110AudioProcessor, compiled against the native
# backend, driven by processBlock() exactly as a DAW would - not a probe that steps
# D110CoreNative directly. This is what caught the missing runForSeconds() call.
add_executable(d110_native_chord_freeze_repro native_chord_freeze_repro.cpp
    Source/native/mcs96_cpu.cpp Source/native/d110_bus.cpp Source/native/msm6222b.cpp
    Source/native/D110CoreNative.cpp Source/native/ram_mirror.cpp)
target_include_directories(d110_native_chord_freeze_repro PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(d110_native_chord_audio_repro native_chord_audio_repro.cpp
    Source/native/mcs96_cpu.cpp Source/native/d110_bus.cpp Source/native/msm6222b.cpp
    Source/native/D110CoreNative.cpp Source/native/ram_mirror.cpp)
target_include_directories(d110_native_chord_audio_repro PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(d110_native_chord_audio_repro PRIVATE MT32Emu::mt32emu)

add_executable(d110_native_polyphony_stress native_polyphony_stress_probe.cpp
    Source/native/mcs96_cpu.cpp Source/native/d110_bus.cpp Source/native/msm6222b.cpp
    Source/native/D110CoreNative.cpp Source/native/ram_mirror.cpp)
target_include_directories(d110_native_polyphony_stress PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# Targeted repro for the specific EXTINT-falling-edge-skipped mechanism found while reading
# serviceStuckPolicy() - see the probe's own header comment.
add_executable(d110_native_ramp_edge_stress native_ramp_edge_stress_probe.cpp
    Source/native/mcs96_cpu.cpp Source/native/d110_bus.cpp Source/native/msm6222b.cpp
    Source/native/D110CoreNative.cpp Source/native/ram_mirror.cpp)
target_include_directories(d110_native_ramp_edge_stress PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(d110_native_plugin_smoke native_plugin_smoke_test.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp
    Source/native/mcs96_cpu.cpp Source/native/d110_bus.cpp Source/native/msm6222b.cpp
    Source/native/D110CoreNative.cpp Source/native/ram_mirror.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp)
target_include_directories(d110_native_plugin_smoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_native_plugin_smoke PRIVATE
    D110_NATIVE_CORE=1
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_native_plugin_smoke PRIVATE
    D110PanelData MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)

# Same shape as d110_native_plugin_smoke - checks whether a TimbreTemp group/number write via
# sendTimbreTempParam genuinely sticks on the real firmware RAM; see the probe's own header
# comment.
add_executable(d110_native_timbre_group_number native_timbre_group_number_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp
    Source/native/mcs96_cpu.cpp Source/native/d110_bus.cpp Source/native/msm6222b.cpp
    Source/native/D110CoreNative.cpp Source/native/ram_mirror.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp)
target_include_directories(d110_native_timbre_group_number PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_native_timbre_group_number PRIVATE
    D110_NATIVE_CORE=1
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_native_timbre_group_number PRIVATE
    D110PanelData MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)

# Same shape as d110_native_plugin_smoke - reproduces Alan's single-part "notes stop getting
# through, Super Mode toggle or a patch change unsticks it" report; see the probe's own
# header comment.
add_executable(d110_native_single_part_stuck native_single_part_stuck_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp
    Source/native/mcs96_cpu.cpp Source/native/d110_bus.cpp Source/native/msm6222b.cpp
    Source/native/D110CoreNative.cpp Source/native/ram_mirror.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp)
target_include_directories(d110_native_single_part_stuck PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_native_single_part_stuck PRIVATE
    D110_NATIVE_CORE=1
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_native_single_part_stuck PRIVATE
    D110PanelData MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)

# Same shape as d110_native_plugin_smoke, but for the sequencer's getStateInformation/
# setStateInformation round trip - see that file's header comment.
add_executable(d110_sequencer_state_probe sequencer_state_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/UiTheme.cpp
    Source/native/mcs96_cpu.cpp Source/native/d110_bus.cpp Source/native/msm6222b.cpp
    Source/native/D110CoreNative.cpp Source/native/ram_mirror.cpp
    Source/sequencer/D110SequencerEngine.cpp Source/sequencer/D110SequencerPanel.cpp)
target_include_directories(d110_sequencer_state_probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_sequencer_state_probe PRIVATE
    D110_NATIVE_CORE=1
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_sequencer_state_probe PRIVATE
    D110PanelData MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)

# Same snapshot idea as d110_editor_shot (see that target's comment), but against the
# native core so it builds without D110_BUILD_MAME_BACKEND - useful for eyeballing LCD
# rendering changes (panel scale, dot antialiasing) without a full MAME configure.
add_executable(d110_native_editor_shot editor_shot.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp
    Source/native/mcs96_cpu.cpp Source/native/d110_bus.cpp Source/native/msm6222b.cpp
    Source/native/D110CoreNative.cpp Source/native/ram_mirror.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp)
target_include_directories(d110_native_editor_shot PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_native_editor_shot PRIVATE
    D110_NATIVE_CORE=1
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_native_editor_shot PRIVATE
    D110PanelData MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)

# The sequencer engine (Source/sequencer/) has no firmware/plugin dependency at all -
# it only knows MIDI channels, beats and sample counts - so its probe links nothing
# beyond the two JUCE modules it actually uses.
add_executable(d110_sequencer_probe sequencer_probe.cpp Source/sequencer/D110SequencerEngine.cpp)
target_include_directories(d110_sequencer_probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_sequencer_probe PRIVATE JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)
target_link_libraries(d110_sequencer_probe PRIVATE juce::juce_audio_basics juce::juce_core)

if(D110_BUILD_MAME_BACKEND)

# One-shot MAME-backed helper: produces a properly factory-reset nvram folder for
# native_note_probe.cpp to load (see that file's header comment).
add_executable(d110_mame_factory_reset_tool mame_factory_reset_tool.cpp Source/D110Core.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_mame_factory_reset_tool PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_mame_factory_reset_tool PRIVATE ${MAME_DEFS})
target_link_libraries(d110_mame_factory_reset_tool PRIVATE ${MAME_LIBS} ${MAME_SYS_LIBS})
target_link_options(d110_mame_factory_reset_tool PRIVATE ${MAME_LINK_OPTIONS})

# Diagnostic: same raw pushMidi()->popNoteEvent() experiment as native_note_probe.cpp, but
# against the real MAME-backed core directly - isolates a native-core bug from a property of
# the raw D110Core interface itself.
add_executable(d110_mame_note_probe mame_note_probe.cpp Source/D110Core.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_mame_note_probe PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_mame_note_probe PRIVATE ${MAME_DEFS})
target_link_libraries(d110_mame_note_probe PRIVATE ${MAME_LIBS} ${MAME_SYS_LIBS})
target_link_options(d110_mame_note_probe PRIVATE ${MAME_LINK_OPTIONS})

endif() # D110_BUILD_MAME_BACKEND
