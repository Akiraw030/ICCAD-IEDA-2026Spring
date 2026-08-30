cmake_minimum_required(VERSION 3.10)
project(cadd0045 LANGUAGES CXX)

# Use the optimized submission configuration by default for single-config
# generators. Developers can still request Debug or RelWithDebInfo explicitly.
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY
        STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ENABLE_OUTPUT "Enable debug output and timing reports" ON)

add_executable(cadd0045
    src/main.cpp
    src/parser.cpp
    src/tree.cpp
    src/delay_model.cpp
    src/fast_timing_engine.cpp
    src/optimizer.cpp
    src/io.cpp
)

target_include_directories(cadd0045 PRIVATE ${CMAKE_SOURCE_DIR}/src)

if(ENABLE_OUTPUT)
    target_compile_definitions(cadd0045 PRIVATE ENABLE_OUTPUT)
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    target_link_libraries(cadd0045 PRIVATE stdc++fs)
endif()
