cmake_minimum_required(VERSION 3.24)
project(minitoolchain
        VERSION 1.0.0
        DESCRIPTION "A complete toolchain for a custom 64-bit virtual ISA"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(MINITOOL_BUILD_TESTS      "Build the test suite"             ON)
option(MINITOOL_BUILD_BENCHMARKS "Build the benchmark suite"        ON)
option(MINITOOL_WERROR           "Treat warnings as errors"         ON)
option(MINITOOL_ENABLE_ASAN      "AddressSanitizer + LeakSanitizer" OFF)
option(MINITOOL_ENABLE_UBSAN     "UndefinedBehaviorSanitizer"       OFF)
option(MINITOOL_ENABLE_TSAN      "ThreadSanitizer"                  OFF)
option(MINITOOL_ENABLE_COVERAGE  "Coverage instrumentation"         OFF)

# ---------------------------------------------------------------- warnings ---
# The project is warning-clean on GCC, Clang and MSVC, and CI keeps it that way
# with -Werror. Warnings get fixed, never suppressed.
add_library(minitool_warnings INTERFACE)
if(MSVC)
    target_compile_options(minitool_warnings INTERFACE
        /W4 /permissive- /Zc:preprocessor /utf-8
        /w14242 /w14254 /w14263 /w14265 /w14287 /w14296 /w14545 /w14546
        /w14547 /w14549 /w14555 /w14619 /w14640 /w14826 /w14905 /w14906 /w14928)
    if(MINITOOL_WERROR)
        target_compile_options(minitool_warnings INTERFACE /WX)
    endif()
    target_compile_definitions(minitool_warnings INTERFACE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(minitool_warnings INTERFACE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow
        -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Wunused
        -Woverloaded-virtual -Wnull-dereference -Wdouble-promotion)
    if(MINITOOL_WERROR)
        target_compile_options(minitool_warnings INTERFACE -Werror)
    endif()
endif()

# ------------------------------------------------------------- sanitizers ---
add_library(minitool_sanitizers INTERFACE)
if(MSVC)
    if(MINITOOL_ENABLE_ASAN)
        target_compile_options(minitool_sanitizers INTERFACE /fsanitize=address)
    endif()
    if(MINITOOL_ENABLE_UBSAN OR MINITOOL_ENABLE_TSAN)
        message(WARNING "UBSan and TSan are not available with MSVC; use clang-cl or a GCC/Clang build")
    endif()
else()
    set(_mt_san "")
    if(MINITOOL_ENABLE_ASAN)
        list(APPEND _mt_san address)
    endif()
    if(MINITOOL_ENABLE_UBSAN)
        list(APPEND _mt_san undefined)
    endif()
    if(MINITOOL_ENABLE_TSAN)
        list(APPEND _mt_san thread)
    endif()
    if(_mt_san)
        list(JOIN _mt_san "," _mt_san_joined)
        target_compile_options(minitool_sanitizers INTERFACE
            -fsanitize=${_mt_san_joined} -fno-omit-frame-pointer -fno-sanitize-recover=all)
        target_link_options(minitool_sanitizers INTERFACE -fsanitize=${_mt_san_joined})
    endif()
    if(MINITOOL_ENABLE_COVERAGE)
        target_compile_options(minitool_sanitizers INTERFACE --coverage -O0 -g)
        target_link_options(minitool_sanitizers INTERFACE --coverage)
    endif()
endif()

# ---------------------------------------------------------- project target ---
add_library(minitool_options INTERFACE)
target_link_libraries(minitool_options INTERFACE minitool_warnings minitool_sanitizers)
target_include_directories(minitool_options INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

add_subdirectory(src)

if(MINITOOL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(MINITOOL_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

message(STATUS "minitoolchain ${PROJECT_VERSION}")
message(STATUS "  build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "  compiler   : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  tests      : ${MINITOOL_BUILD_TESTS}")
message(STATUS "  benchmarks : ${MINITOOL_BUILD_BENCHMARKS}")
message(STATUS "  asan/ubsan : ${MINITOOL_ENABLE_ASAN}/${MINITOOL_ENABLE_UBSAN}")
