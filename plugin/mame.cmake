# The MAME d110 subset: the real Roland firmware, its menus, its MSM6222B LCD and its
# 16 front-panel buttons. Built out of the MAME 0.288 tree with
#   make vs2022 MSBUILD=1 PTR64=1 MINGW64=... NOWERROR=1 PYTHON_EXECUTABLE=python \
#        SUBTARGET=d110 SOURCES=src/mame/roland/roland_d10.cpp USE_BGFX=0
# No MAME source patches are needed: render(), the "rams" memory share and the SC0/SC1
# ioports are all public. See docs/sysex_address_map.md.
set(MAME_DIR "C:/Users/bd260/Projects/MU128-VST/mame-master")
set(_mame_libdir  "${MAME_DIR}/build/vs2022/bin/x64/Release")
set(_mame_sublib  "${_mame_libdir}/mame_d110")

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

set(MAME_DRIVLIST "${MAME_DIR}/build/generated/mame/d110/drivlist.cpp")
