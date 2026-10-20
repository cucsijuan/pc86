cmake_minimum_required(VERSION 3.10)
project(kbd86 C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(MSVC)
  add_compile_options(/W4 /utf-8)
else()
  add_compile_options(-Wall -Wextra -pedantic)
endif()

add_library(kbd86 STATIC src/kbd.c src/pic.c)
target_include_directories(kbd86 PUBLIC include)

add_library(kbd86_shared SHARED src/kbd.c src/pic.c)
target_include_directories(kbd86_shared PUBLIC include)
target_compile_definitions(kbd86_shared PRIVATE KBD86_BUILD_DLL)
set_target_properties(kbd86_shared PROPERTIES OUTPUT_NAME kbd86)

add_executable(kbd_test tests/kbd_test.c)
target_link_libraries(kbd_test kbd86)

enable_testing()
add_test(NAME kbd COMMAND kbd_test)

set(VM86_DIR "" CACHE PATH "Path to the vm86 package (optional)")
if(VM86_DIR)
  add_executable(integration_test tests/integration_test.c ${VM86_DIR}/src/cpu.c ${VM86_DIR}/src/asm.c)
  target_include_directories(integration_test PRIVATE ${VM86_DIR}/include)
  target_link_libraries(integration_test kbd86)
  add_test(NAME integration COMMAND integration_test)
endif()
