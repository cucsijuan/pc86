cmake_minimum_required(VERSION 3.10)
project(pc86 C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(MSVC)
  add_compile_options(/W4 /utf-8)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-Wall -Wextra -pedantic)
endif()

# The other components, next to this one by default
set(VM86_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../vm86 CACHE PATH "vm86 package")
set(PIT86_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../pit86 CACHE PATH "pit86 package")
set(KBD86_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../kbd86 CACHE PATH "kbd86 package")
set(VGA86_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../vga86 CACHE PATH "vga86 package")

set(PC86_SOURCES
  src/pc.c src/pc_vga.c
  ${VM86_DIR}/src/cpu.c ${VM86_DIR}/src/asm.c
  ${PIT86_DIR}/src/pit.c
  ${KBD86_DIR}/src/kbd.c ${KBD86_DIR}/src/kbc.c ${KBD86_DIR}/src/pic.c
  ${VGA86_DIR}/src/vga.c ${VGA86_DIR}/src/monitor.c ${VGA86_DIR}/src/font_cp437.c)
set(PC86_INCLUDES include ${VM86_DIR}/include ${PIT86_DIR}/include ${KBD86_DIR}/include ${VGA86_DIR}/include)

# The whole machine in one library (static, and shared for GDExtension and friends)
add_library(pc86 STATIC ${PC86_SOURCES})
target_include_directories(pc86 PUBLIC ${PC86_INCLUDES})

add_library(pc86_shared SHARED ${PC86_SOURCES})
target_include_directories(pc86_shared PUBLIC ${PC86_INCLUDES})
target_compile_definitions(pc86_shared PRIVATE PC86_BUILD_DLL VM86_BUILD_DLL PIT86_BUILD_DLL KBD86_BUILD_DLL VGA86_BUILD_DLL)
set_target_properties(pc86_shared PROPERTIES OUTPUT_NAME pc86)

if(NOT MSVC)
  target_link_libraries(pc86 PUBLIC m)
  target_link_libraries(pc86_shared PUBLIC m)
endif()

add_executable(pcrun tools/pcrun.c)
target_link_libraries(pcrun pc86)

add_executable(pc_test tests/pc_test.c)
target_link_libraries(pc_test pc86)

enable_testing()
add_test(NAME pc COMMAND pc_test)
