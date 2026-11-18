cmake_minimum_required(VERSION 3.22)

project(MartingaleExchange
    VERSION 0.1.0
    DESCRIPTION "High-Performance Electronic Trading Exchange and Quantitative Research Platform"
    LANGUAGES CXX
)

# Enforce strict C++20 across all targets
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile_commands.json for static analysis and IDE tooling
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build configuration options
option(MARTINGALE_BUILD_TESTS "Build GoogleTest unit and integration test suite" ON)
option(MARTINGALE_BUILD_BENCHMARKS "Build Google Benchmark performance suite" OFF)
option(MARTINGALE_ENABLE_ASAN "Enable AddressSanitizer (ASan)" OFF)
option(MARTINGALE_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer (UBSan)" OFF)
option(MARTINGALE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors (-Werror)" ON)

# Dedicated INTERFACE target for project-owned compiler warnings and sanitizers.
# Kept separate from global flags so third-party FetchContent dependencies
# (such as GoogleTest) do not fail under -Werror or strict conversion flags.
add_library(martingale_options INTERFACE)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(martingale_options INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wcast-align
        -Wnon-virtual-dtor
        -Woverloaded-virtual
        -Wnull-dereference
        -Wformat=2
    )
    if(MARTINGALE_WARNINGS_AS_ERRORS)
        target_compile_options(martingale_options INTERFACE -Werror)
    endif()
endif()

if(MARTINGALE_ENABLE_ASAN)
    target_compile_options(martingale_options INTERFACE
        -fsanitize=address
        -fno-omit-frame-pointer
    )
    target_link_options(martingale_options INTERFACE
        -fsanitize=address
    )
endif()

if(MARTINGALE_ENABLE_UBSAN)
    target_compile_options(martingale_options INTERFACE
        -fsanitize=undefined
        -fno-sanitize-recover=all
        -fno-omit-frame-pointer
    )
    target_link_options(martingale_options INTERFACE
        -fsanitize=undefined
    )
endif()

# Core exchange library (header-only INTERFACE target during foundation phase)
add_library(martingale_core INTERFACE)
target_include_directories(martingale_core INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(martingale_core INTERFACE cxx_std_20)
target_link_libraries(martingale_core INTERFACE martingale_options)

if(MARTINGALE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
