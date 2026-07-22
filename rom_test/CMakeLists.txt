cmake_minimum_required(VERSION 3.15)
project(rom_test CXX)

set(BUILD_SHARED_LIBS FALSE CACHE BOOL "" FORCE)
set(libmt32emu_SHARED FALSE CACHE BOOL "" FORCE)
set(libmt32emu_C_INTERFACE FALSE CACHE BOOL "" FORCE)
set(libmt32emu_CPP_INTERFACE TRUE CACHE BOOL "" FORCE)

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../munt/mt32emu" "${CMAKE_CURRENT_BINARY_DIR}/mt32emu_build")

add_executable(rom_test main.cpp)
target_link_libraries(rom_test PRIVATE MT32Emu::mt32emu)
set_property(TARGET rom_test PROPERTY CXX_STANDARD 14)
