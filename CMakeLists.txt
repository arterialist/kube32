# ==============================================================================
# Kube32 - Kubernetes on ESP32 Microcontrollers
# Top-Level Dual-Target CMakeLists.txt
# ==============================================================================
cmake_minimum_required(VERSION 3.16.0)

# ------------------------------------------------------------------------------
# Target Detection: ESP-IDF vs Host Simulation
# ------------------------------------------------------------------------------
if(DEFINED ENV{IDF_PATH} OR DEFINED ESP_PLATFORM OR DEFINED PLATFORMIO_BUILD_DIR)
    set(KUBE32_IS_ESP32 TRUE)
else()
    set(KUBE32_IS_ESP32 FALSE)
endif()

if(KUBE32_IS_ESP32)
    # ==========================================================================
    # ESP-IDF Target (ESP32-S3 Microcontroller Firmware)
    # ==========================================================================
    message(STATUS "[Kube32] Building for Target: ESP32-S3 (ESP-IDF / FreeRTOS)")
    
    include($ENV{IDF_PATH}/tools/cmake/project.cmake)
    project(kube32)

else()
    # ==========================================================================
    # Host Target (macOS Apple Clang / Linux GCC Simulation)
    # ==========================================================================
    message(STATUS "[Kube32] Building for Target: Host Simulation (macOS / Linux)")
    
    project(kube32 LANGUAGES C CXX)

    # Enforce C++17 Standard
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

    # Compiler Warning & Optimization Flags
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang|GNU")
        add_compile_options(
            -Wall
            -Wextra
            -Wpedantic
            -Werror=return-type
            -Wno-unused-parameter
            -pthread
        )
    endif()

    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose Release or Debug" FORCE)
    endif()

    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_compile_options(-g -O0 -DDEBUG)
    else()
        add_compile_options(-O2 -DNDEBUG)
    endif()

    # Preprocessor Target Flags
    add_compile_definitions(KUBE32_TARGET_HOST=1 KUBE32_HOST_SIM=1)

    # Core Include Directories
    include_directories(
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    # Core Library: OSAL, Memory Architecture, Storage Engine
    set(KUBE32_CORE_SRCS
        src/osal/thread_posix.cpp
        src/osal/mutex_posix.cpp
        src/osal/condition_variable_posix.cpp
        src/osal/timer_posix.cpp
        src/osal/socket_posix.cpp
        src/osal/system_posix.cpp
        src/mem/memory_tier.cpp
        src/mem/monotonic_arena.cpp
        src/storage/radix_tree.cpp
        src/storage/littlefs_journal.cpp
        src/storage/store.cpp
    )

    add_library(kube32_core STATIC
        ${KUBE32_CORE_SRCS}
    )
    target_include_directories(kube32_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(kube32_core PUBLIC pthread)

    # Optional Simulation Binary (if main_host.cpp is present)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/main_host.cpp")
        add_executable(kube32-sim
            src/main_host.cpp
        )
        target_link_libraries(kube32-sim PRIVATE kube32_core)
    endif()

    # ==========================================================================
    # Unit Test Targets & CTest Registration
    # ==========================================================================
    enable_testing()

    # 1. OSAL Unit & Concurrency Tests
    add_executable(test_osal
        tests/test_osal.cpp
    )
    target_link_libraries(test_osal PRIVATE kube32_core)
    add_test(NAME test_osal COMMAND test_osal)

    # 2. Memory Architecture Baseline Tests
    add_executable(test_memory
        tests/test_memory.cpp
    )
    target_link_libraries(test_memory PRIVATE kube32_core)
    add_test(NAME test_memory COMMAND test_memory)

    # 3. Memory Architecture Adversarial Stress & Hardening Tests
    add_executable(test_memory_adversarial
        tests/test_memory_adversarial.cpp
    )
    target_link_libraries(test_memory_adversarial PRIVATE kube32_core)
    add_test(NAME test_memory_adversarial COMMAND test_memory_adversarial)

    # 4. Storage Engine Baseline Tests
    add_executable(test_storage
        tests/test_storage.cpp
    )
    target_link_libraries(test_storage PRIVATE kube32_core)
    add_test(NAME test_storage COMMAND test_storage)

    # 5. Storage Engine Adversarial Stress & Hardening Tests
    add_executable(test_storage_adversarial
        tests/test_storage_adversarial.cpp
    )
    target_link_libraries(test_storage_adversarial PRIVATE kube32_core)
    add_test(NAME test_storage_adversarial COMMAND test_storage_adversarial)

endif()
