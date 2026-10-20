cmake_minimum_required(VERSION 3.10)
project(vga86 C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(MSVC)
  add_compile_options(/W4 /utf-8)
else()
  add_compile_options(-Wall -Wextra -pedantic)
endif()

add_library(vga86 STATIC src/vga.c src/font_cp437.c)
target_include_directories(vga86 PUBLIC include)
if(NOT MSVC)
  target_link_libraries(vga86 PUBLIC m)
endif()

add_library(vga86_shared SHARED src/vga.c src/font_cp437.c)
target_include_directories(vga86_shared PUBLIC include)
target_compile_definitions(vga86_shared PRIVATE VGA86_BUILD_DLL)
set_target_properties(vga86_shared PROPERTIES OUTPUT_NAME vga86)
if(NOT MSVC)
  target_link_libraries(vga86_shared PUBLIC m)
endif()

add_executable(vga_test tests/vga_test.c)
target_link_libraries(vga_test vga86)

enable_testing()
add_test(NAME vga COMMAND vga_test)

# Optional: integration test and demo runner with the vm86 package
set(VM86_DIR "" CACHE PATH "Path to the vm86 package (optional)")
if(VM86_DIR)
  add_executable(integration_test tests/integration_test.c
    ${VM86_DIR}/src/cpu.c ${VM86_DIR}/src/asm.c)
  target_include_directories(integration_test PRIVATE ${VM86_DIR}/include)
  target_link_libraries(integration_test vga86)
  add_test(NAME integration COMMAND integration_test)

  add_executable(vgarun examples/vm86_glue.c ${VM86_DIR}/src/cpu.c)
  target_include_directories(vgarun PRIVATE ${VM86_DIR}/include)
  target_link_libraries(vgarun vga86)
endif()
