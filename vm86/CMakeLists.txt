cmake_minimum_required(VERSION 3.10)
project(vm86 C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(MSVC)
  add_compile_options(/W4 /utf-8)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-Wall -Wextra -pedantic)
endif()

# Library: CPU core + assembler
add_library(vm86 STATIC src/cpu.c src/asm.c)
target_include_directories(vm86 PUBLIC include)

# Shared version (for P/Invoke in Unity, GDExtension, etc.)
add_library(vm86_shared SHARED src/cpu.c src/asm.c)
target_include_directories(vm86_shared PUBLIC include)
target_compile_definitions(vm86_shared PRIVATE VM86_BUILD_DLL)
set_target_properties(vm86_shared PROPERTIES OUTPUT_NAME vm86)

add_executable(asm86 tools/asm86_cli.c)
target_link_libraries(asm86 vm86)

add_executable(run86 tools/run86.c)
target_link_libraries(run86 vm86)

add_executable(api_test tests/api_test.c)
target_link_libraries(api_test vm86)

enable_testing()
add_test(NAME api COMMAND api_test)
