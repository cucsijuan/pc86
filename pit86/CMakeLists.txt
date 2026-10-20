cmake_minimum_required(VERSION 3.10)
project(pit86 C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(MSVC)
  add_compile_options(/W4 /utf-8)
else()
  add_compile_options(-Wall -Wextra -pedantic)
endif()

add_library(pit86 STATIC src/pit.c)
target_include_directories(pit86 PUBLIC include)

add_library(pit86_shared SHARED src/pit.c)
target_include_directories(pit86_shared PUBLIC include)
target_compile_definitions(pit86_shared PRIVATE PIT86_BUILD_DLL)
set_target_properties(pit86_shared PROPERTIES OUTPUT_NAME pit86)

add_executable(pit_test tests/pit_test.c)
target_link_libraries(pit_test pit86)

enable_testing()
add_test(NAME pit COMMAND pit_test)
