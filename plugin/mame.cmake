# The MAME d110 subset: the real Roland firmware, its menus, its MSM6222B LCD and its
# 16 front-panel buttons. No MAME source patches are needed: render(), the "rams" memory
# share and the SC0/SC1 ioports are all public. See docs/sysex_address_map.md.
#
# Windows: built out of the MAME 0.288 tree with
#   make vs2022 MSBUILD=1 PTR64=1 MINGW64=... NOWERROR=1 PYTHON_EXECUTABLE=python \
#        SUBTARGET=d110 SOURCES=src/mame/roland/roland_d10.cpp USE_BGFX=0
# Linux: built out of the same mame0288 tag with GNU make and the SDL OSD (MAME's own
# default OSD on Linux), no patches, no extra flags:
#   make SUBTARGET=d110 SOURCES=src/mame/roland/roland_d10.cpp -j$(nproc)
# Verified to build cleanly this way and to produce every archive the Windows build
# lists below, just as .a instead of .lib, plus a working standalone `d110` executable.
# macOS: UNPROVEN, worked out empirically on a macos-latest GitHub Actions runner (this
# fork has no Mac to test on). Built with the same GNU make invocation as Linux - MAME's
# GENie build system supports macOS natively via its own "mac_clang" OSD/toolchain, no
# extra flags needed beyond SDL2/SDL2_ttf from Homebrew:
#   brew install sdl2 sdl2_ttf
#   make SUBTARGET=d110 SOURCES=src/mame/roland/roland_d10.cpp -j$(sysctl -n hw.ncpu)
# See .github/workflows/build-macos.yml for exactly what was tried and what broke, if
# anything did - that workflow's history is the ground truth here, not this comment.
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(MAME_DIR "C:/Users/bd260/Projects/MU128-VST/mame-master")
    set(_mame_libdir "${MAME_DIR}/build/vs2022/bin/x64/Release")
    set(_mame_sublib "${_mame_libdir}/mame_d110")

    set(MAME_LIBS
      "${_mame_sublib}/mame_d110.lib" "${_mame_sublib}/optional.lib"
      "${_mame_sublib}/formats.lib"   "${_mame_sublib}/dasm.lib"
      "${_mame_libdir}/frontend.lib"  "${_mame_libdir}/emu.lib"
      "${_mame_libdir}/osd_windows.lib" "${_mame_libdir}/ocore_windows.lib"
      "${_mame_libdir}/qtdbg_windows.lib" "${_mame_libdir}/utils.lib"
      "${_mame_libdir}/expat.lib" "${_mame_libdir}/zlib.lib"
      "${_mame_libdir}/portmidi.lib" "${_mame_libdir}/flac.lib" "${_mame_libdir}/7z.lib"
      "${_mame_libdir}/softfloat3.lib" "${_mame_libdir}/utf8proc.lib"
      "${_mame_libdir}/jpeg.lib" "${_mame_libdir}/sqlite3.lib" "${_mame_libdir}/zstd.lib"
      "${_mame_libdir}/portaudio.lib" "${_mame_libdir}/lua.lib" "${_mame_libdir}/lualibs.lib"
      "${_mame_libdir}/asmjit.lib" "${_mame_libdir}/bgfx.lib" "${_mame_libdir}/bimg.lib"
      "${_mame_libdir}/bx.lib" "${_mame_libdir}/ymfm.lib" "${_mame_libdir}/wdlfft.lib"
      "${_mame_libdir}/linenoise.lib")
    # No ntdll.lib - it is imported by ordinal and makes the module fail to load.
    # No softfloat.lib - merged into softfloat3.lib on this tree.

    set(MAME_SYS_LIBS ws2_32 winmm dinput8 dxguid opengl32 dsound xinput
                      comctl32 shlwapi userenv bcrypt advapi32 gdi32 user32 ole32
                      oleaut32 uuid shcore)

    set(MAME_INCLUDES
      "${MAME_DIR}/src" "${MAME_DIR}/src/osd" "${MAME_DIR}/src/emu"
      "${MAME_DIR}/src/devices" "${MAME_DIR}/src/mame" "${MAME_DIR}/src/mame/shared"
      "${MAME_DIR}/src/lib" "${MAME_DIR}/src/lib/util" "${MAME_DIR}/src/lib/netlist"
      "${MAME_DIR}/3rdparty" "${MAME_DIR}/build/generated/mame/layout"
      "${MAME_DIR}/3rdparty/asio/include" "${MAME_DIR}/3rdparty/flac/include"
      "${MAME_DIR}/3rdparty/glm" "${MAME_DIR}/3rdparty/libjpeg"
      "${MAME_DIR}/3rdparty/rapidjson/include" "${MAME_DIR}/3rdparty/zlib"
      "${MAME_DIR}/3rdparty/dxsdk/Include")

    set(MAME_DEFS
      CRLF=3 LSB_FIRST FLAC__NO_DLL PUGIXML_HEADER_ONLY ASMJIT_STATIC
      LUA_COMPAT_ALL LUA_COMPAT_5_1 LUA_COMPAT_5_2 XML_STATIC
      _CRT_NONSTDC_NO_DEPRECATE _CRT_SECURE_NO_DEPRECATE
      _CRT_STDIO_LEGACY_WIDE_SPECIFIERS MAME_DEBUG MAME_PROFILER
      __STDC_FORMAT_MACROS __STDC_CONSTANT_MACROS)

    set(MAME_LINK_OPTIONS /ignore:4099 /FORCE:MULTIPLE)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    # UNVERIFIED beyond what CI has actually exercised - see the comment at the top of this
    # file. The output directory name (mac_clang), the lib set, and MAME_SYS_LIBS below are
    # a best-effort port of the Linux branch, corrected against whatever build-macos.yml's
    # runs actually produced; treat any part of this branch that the workflow never reached
    # as a guess, not a verified fact.
    set(MAME_DIR "$ENV{HOME}/src/D110/mame-build/mame" CACHE PATH
        "Path to a mame0288 checkout with SUBTARGET=d110 already built for macOS (see above)")
    # Confirmed by an actual macos-latest run (build-macos.yml), NOT a guess: the output
    # dir is osx_clang (not mac_clang), and MAME 0.288's macOS OSD is SDL3-based - GENie
    # generates build/projects/sdl3/mamed110, and the archives below are named *_sdl3.
    set(_mame_libdir "${MAME_DIR}/build/osx_clang/bin/x64/Release")
    set(_mame_sublib "${_mame_libdir}/mame_d110")

    # Same --start-group/--end-group reasoning as the Linux branch: MAME's own libs
    # cross-reference each other and a single left-to-right pass can't order them all.
    set(MAME_LIBS
      -Wl,-force_load,"${_mame_sublib}/libmame_d110.a"
      "${_mame_sublib}/liboptional.a"
      "${_mame_sublib}/libformats.a"   "${_mame_sublib}/libdasm.a"
      "${_mame_libdir}/libfrontend.a"  "${_mame_libdir}/libemu.a"
      "${_mame_libdir}/libosd_sdl3.a" "${_mame_libdir}/libocore_sdl3.a"
      "${_mame_libdir}/libqtdbg_sdl3.a" "${_mame_libdir}/libutils.a"
      "${_mame_libdir}/libexpat.a" "${_mame_libdir}/libzlib.a"
      "${_mame_libdir}/libportmidi.a" "${_mame_libdir}/libflac.a" "${_mame_libdir}/lib7z.a"
      "${_mame_libdir}/libsoftfloat3.a" "${_mame_libdir}/libutf8proc.a"
      "${_mame_libdir}/libjpeg.a" "${_mame_libdir}/libsqlite3.a" "${_mame_libdir}/libzstd.a"
      "${_mame_libdir}/libportaudio.a" "${_mame_libdir}/liblua.a" "${_mame_libdir}/liblualibs.a"
      "${_mame_libdir}/libasmjit.a" "${_mame_libdir}/libbgfx.a" "${_mame_libdir}/libbimg.a"
      "${_mame_libdir}/libbx.a" "${_mame_libdir}/libymfm.a" "${_mame_libdir}/libwdlfft.a"
      "${_mame_libdir}/liblinenoise.a" "${_mame_libdir}/libprecompile.a")
    # macOS's ld64 has no --start-group/--end-group; -force_load on the one archive whose
    # symbols (driver_list::s_driver_count etc) nothing else in this list pulls in on its
    # own is ld64's equivalent of the Linux --whole-archive workaround CMakeLists.txt
    # applies to D110Emulator's own SharedCode archive.

    # Apple frameworks replacing the Linux system libs - CoreAudio/CoreMIDI/AudioToolbox for
    # sound and MIDI I/O, IOKit for device enumeration, Cocoa/QuartzCore/Metal for the SDL
    # OSD's window and GL/Metal context, Carbon for legacy HID bits SDL2 still touches.
    # UNVERIFIED: derived from what MAME's own macOS OSD (src/osd/sdl) links against, not
    # from an actual `otool -L`/linker-error pass on this tree - correct against the real
    # CI failures in build-macos.yml before trusting this list.
    find_library(COREAUDIO_LIB CoreAudio REQUIRED)
    find_library(COREMIDI_LIB CoreMIDI REQUIRED)
    find_library(COREVIDEO_LIB CoreVideo REQUIRED)
    find_library(AUDIOTOOLBOX_LIB AudioToolbox REQUIRED)
    find_library(AUDIOUNIT_LIB AudioUnit REQUIRED)
    find_library(IOKIT_LIB IOKit REQUIRED)
    find_library(COCOA_LIB Cocoa REQUIRED)
    find_library(QUARTZCORE_LIB QuartzCore REQUIRED)
    find_library(METAL_LIB Metal REQUIRED)
    find_library(CARBON_LIB Carbon REQUIRED)
    find_library(FOUNDATION_LIB Foundation REQUIRED)
    # Confirmed by CI: renderer_ogl in libosd_sdl3.a calls the legacy fixed-function GL API
    # directly (glBegin/glEnd and friends) - MAME's SDL OSD still has an OpenGL renderer
    # option on macOS alongside Metal, and it's apparently what got built here.
    find_library(OPENGL_LIB OpenGL REQUIRED)
    # Confirmed by CI: MAME 0.288's macOS OSD links against SDL3, not SDL2 - the archives
    # above are literally named *_sdl3. Homebrew's sdl2 formula is sdl2-compat, which pulls
    # in sdl3 as a dependency, but that is a coincidence of the CI image, not something to
    # rely on - install sdl3 (and sdl3_ttf, if MAME turns out to need it - unconfirmed,
    # carried over from the Linux branch's SDL2_ttf and not yet proven required here) directly.
    find_package(SDL3 REQUIRED)

    set(MAME_SYS_LIBS
      ${COREAUDIO_LIB} ${COREMIDI_LIB} ${COREVIDEO_LIB} ${AUDIOTOOLBOX_LIB}
      ${AUDIOUNIT_LIB} ${IOKIT_LIB} ${COCOA_LIB} ${QUARTZCORE_LIB} ${METAL_LIB}
      ${CARBON_LIB} ${FOUNDATION_LIB} ${OPENGL_LIB}
      SDL3::SDL3)

    set(MAME_INCLUDES
      "${MAME_DIR}/src" "${MAME_DIR}/src/osd" "${MAME_DIR}/src/emu"
      "${MAME_DIR}/src/devices" "${MAME_DIR}/src/mame" "${MAME_DIR}/src/mame/shared"
      "${MAME_DIR}/src/lib" "${MAME_DIR}/src/lib/util" "${MAME_DIR}/src/lib/netlist"
      "${MAME_DIR}/3rdparty" "${MAME_DIR}/build/generated/mame/layout"
      "${MAME_DIR}/3rdparty/asio/include" "${MAME_DIR}/3rdparty/flac/include"
      "${MAME_DIR}/3rdparty/glm" "${MAME_DIR}/3rdparty/libjpeg"
      "${MAME_DIR}/3rdparty/rapidjson/include" "${MAME_DIR}/3rdparty/zlib")

    set(MAME_DEFS
      CRLF=3 LSB_FIRST FLAC__NO_DLL PUGIXML_HEADER_ONLY ASMJIT_STATIC
      LUA_COMPAT_ALL LUA_COMPAT_5_1 LUA_COMPAT_5_2 XML_STATIC
      MAME_DEBUG MAME_PROFILER __STDC_FORMAT_MACROS __STDC_CONSTANT_MACROS)

    set(MAME_LINK_OPTIONS "")
else()
    # Override with -DMAME_DIR=... if your own mame0288 checkout lives elsewhere.
    set(MAME_DIR "$ENV{HOME}/src/D110/mame-build/mame" CACHE PATH
        "Path to a mame0288 checkout with SUBTARGET=d110 already built (see above)")
    set(_mame_libdir "${MAME_DIR}/build/linux_gcc/bin/x64/Release")
    set(_mame_sublib "${_mame_libdir}/mame_d110")

    # Wrapped in --start-group/--end-group: MAME's own libs cross-reference each other's
    # symbols (e.g. liblualibs.a's sqlite3 binding needs libsqlite3.a) in an order a single
    # left-to-right GNU ld pass can't satisfy no matter how these are sorted, so let the
    # linker iterate instead of trying to hand-order 30 archives correctly.
    set(MAME_LIBS
      -Wl,--start-group
      "${_mame_sublib}/libmame_d110.a" "${_mame_sublib}/liboptional.a"
      "${_mame_sublib}/libformats.a"   "${_mame_sublib}/libdasm.a"
      "${_mame_libdir}/libfrontend.a"  "${_mame_libdir}/libemu.a"
      "${_mame_libdir}/libosd_sdl.a" "${_mame_libdir}/libocore_sdl.a"
      "${_mame_libdir}/libqtdbg_sdl.a" "${_mame_libdir}/libutils.a"
      "${_mame_libdir}/libexpat.a" "${_mame_libdir}/libzlib.a"
      "${_mame_libdir}/libportmidi.a" "${_mame_libdir}/libflac.a" "${_mame_libdir}/lib7z.a"
      "${_mame_libdir}/libsoftfloat3.a" "${_mame_libdir}/libutf8proc.a"
      "${_mame_libdir}/libjpeg.a" "${_mame_libdir}/libsqlite3.a" "${_mame_libdir}/libzstd.a"
      "${_mame_libdir}/libportaudio.a" "${_mame_libdir}/liblua.a" "${_mame_libdir}/liblualibs.a"
      "${_mame_libdir}/libasmjit.a" "${_mame_libdir}/libbgfx.a" "${_mame_libdir}/libbimg.a"
      "${_mame_libdir}/libbx.a" "${_mame_libdir}/libymfm.a" "${_mame_libdir}/libwdlfft.a"
      "${_mame_libdir}/liblinenoise.a" "${_mame_libdir}/libprecompile.a"
      -Wl,--end-group)

    # Direct link deps of the SDL OSD + Qt debugger + pulse/pipewire sound drivers, per
    # `ldd` on the standalone `d110` built above - everything else (X11 sub-libs, Wayland,
    # ICU, codec libs, etc) is a transitive dependency the dynamic linker resolves on its own.
    set(MAME_SYS_LIBS pthread dl SDL2 SDL2_ttf GL asound pulse pipewire-0.3
                      Qt6Core Qt6Gui Qt6Widgets Qt6DBus X11 Xi fontconfig)

    set(MAME_INCLUDES
      "${MAME_DIR}/src" "${MAME_DIR}/src/osd" "${MAME_DIR}/src/emu"
      "${MAME_DIR}/src/devices" "${MAME_DIR}/src/mame" "${MAME_DIR}/src/mame/shared"
      "${MAME_DIR}/src/lib" "${MAME_DIR}/src/lib/util" "${MAME_DIR}/src/lib/netlist"
      "${MAME_DIR}/3rdparty" "${MAME_DIR}/build/generated/mame/layout"
      "${MAME_DIR}/3rdparty/asio/include" "${MAME_DIR}/3rdparty/flac/include"
      "${MAME_DIR}/3rdparty/glm" "${MAME_DIR}/3rdparty/libjpeg"
      "${MAME_DIR}/3rdparty/rapidjson/include" "${MAME_DIR}/3rdparty/zlib")

    set(MAME_DEFS
      CRLF=3 LSB_FIRST FLAC__NO_DLL PUGIXML_HEADER_ONLY ASMJIT_STATIC
      LUA_COMPAT_ALL LUA_COMPAT_5_1 LUA_COMPAT_5_2 XML_STATIC
      MAME_DEBUG MAME_PROFILER __STDC_FORMAT_MACROS __STDC_CONSTANT_MACROS)

    # GNU ld equivalent of Windows' /FORCE:MULTIPLE: D110Core.cpp provides its own
    # emulator_info:: overrides that deliberately collide with libfrontend.a's defaults
    # (same reason the Windows build needs to ignore the clash) - first definition in link
    # order wins, which is ours since it's listed before ${MAME_LIBS} everywhere.
    set(MAME_LINK_OPTIONS -Wl,--allow-multiple-definition)
endif()

set(MAME_DRIVLIST "${MAME_DIR}/build/generated/mame/d110/drivlist.cpp")
