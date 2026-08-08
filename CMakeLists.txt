cmake_minimum_required(VERSION 3.20)
project(ZLR5401 C ASM)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

execute_process(
  COMMAND ${CMAKE_COMMAND} -E create_symlink
  ${CMAKE_BINARY_DIR}/compile_commands.json
  ${CMAKE_SOURCE_DIR}/compile_commands.json
)

set(CMAKE_TOOLCHAIN_FILE ${CMAKE_SOURCE_DIR}/toolchain-arm-none-eabi.cmake)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/Build)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/Build)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/Build)

add_subdirectory(Application)
add_subdirectory(Bootloader)
