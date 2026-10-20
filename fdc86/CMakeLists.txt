cmake_minimum_required(VERSION 3.10)
project(fdc86 C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(MSVC)
  add_compile_options(/W4 /utf-8)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-Wall -Wextra -pedantic)
endif()

set(FDC86_SOURCES src/fdc.c src/fdc_bios.c src/fdd.c src/fat12.c)

add_library(fdc86 STATIC ${FDC86_SOURCES})
target_include_directories(fdc86 PUBLIC include)

add_library(fdc86_shared SHARED ${FDC86_SOURCES})
target_include_directories(fdc86_shared PUBLIC include)
target_compile_definitions(fdc86_shared PRIVATE FDC86_BUILD_DLL)
set_target_properties(fdc86_shared PROPERTIES OUTPUT_NAME fdc86)

add_executable(fdimage tools/fdimage.c)
target_link_libraries(fdimage fdc86)

add_executable(fdc_test tests/fdc_test.c)
target_link_libraries(fdc_test fdc86)

enable_testing()
add_test(NAME fdc COMMAND fdc_test)
