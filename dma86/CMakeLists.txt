cmake_minimum_required(VERSION 3.10)
project(dma86 C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(MSVC)
  add_compile_options(/W4 /utf-8)
else()
  add_compile_options(-Wall -Wextra -pedantic)
endif()

add_library(dma86 STATIC src/dma.c)
target_include_directories(dma86 PUBLIC include)

add_library(dma86_shared SHARED src/dma.c)
target_include_directories(dma86_shared PUBLIC include)
target_compile_definitions(dma86_shared PRIVATE DMA86_BUILD_DLL)
set_target_properties(dma86_shared PROPERTIES OUTPUT_NAME dma86)

add_executable(dma_test tests/dma_test.c)
target_link_libraries(dma_test dma86)

enable_testing()
add_test(NAME dma COMMAND dma_test)
