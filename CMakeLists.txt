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
            -Wno-c99-extensions
            -Wno-nested-anon-types
            -pthread
        )
    endif()

    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose Release or Debug" FORCE)
    endif()

    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_compile_options(-g -O0 -DDEBUG)
    else()
        add_compile_options(-O2 -UNDEBUG)
    endif()

    # Preprocessor Target Flags
    add_compile_definitions(KUBE32_TARGET_HOST=1 KUBE32_HOST_SIM=1)

    # Core Include Directories
    include_directories(
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/wasm3/source
    )

    # Wasm3 Embedded WebAssembly Runtime Engine
    set(WASM3_SRCS
        third_party/wasm3/source/m3_api_libc.c
        third_party/wasm3/source/m3_api_tracer.c
        third_party/wasm3/source/m3_bind.c
        third_party/wasm3/source/m3_code.c
        third_party/wasm3/source/m3_compile.c
        third_party/wasm3/source/m3_core.c
        third_party/wasm3/source/m3_deterministic.c
        third_party/wasm3/source/m3_env.c
        third_party/wasm3/source/m3_exec.c
        third_party/wasm3/source/m3_function.c
        third_party/wasm3/source/m3_info.c
        third_party/wasm3/source/m3_module.c
        third_party/wasm3/source/m3_parse.c
        third_party/wasm3/source/m3_snapshot.c
        third_party/wasm3/source/m3_validate.c
        third_party/wasm3/source/m3_xxh64.c
    )
    add_library(wasm3 STATIC ${WASM3_SRCS})
    target_include_directories(wasm3 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/wasm3/source)
    target_compile_definitions(wasm3 PUBLIC d_m3CodePageAlignSize=4096)

    # Core Library: OSAL, Memory Architecture, Storage Engine, API Server, Scheduler, HAL, Runtime, Kubelet
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
        src/models/kube_models.cpp
        src/api/table_serializer.cpp
        src/api/route_handler.cpp
        src/api/http_server.cpp
        src/scheduler/scheduler.cpp
        src/hal/capability_firewall.cpp
        src/hal/mock_hal.cpp
        src/runtime/host_functions.cpp
        src/runtime/container_lifecycle.cpp
        src/runtime/wasm_runtime.cpp
        src/kubelet/node_status_reporter.cpp
        src/kubelet/kube_api_client.cpp
        src/kubelet/kubelet.cpp
    )

    add_library(kube32_core STATIC
        ${KUBE32_CORE_SRCS}
    )
    target_include_directories(kube32_core PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/wasm3/source
    )
    target_link_libraries(kube32_core PUBLIC wasm3 pthread)

    # Simulation Binary (kube32-sim)
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

    # 6. Kubernetes API Server & Route Handler Tests (Milestone 2)
    add_executable(test_api_server
        tests/test_api_server.cpp
    )
    target_link_libraries(test_api_server PRIVATE kube32_core)
    add_test(NAME test_api_server COMMAND test_api_server)

    # 7. Autonomous Scheduler & Predicate Tests (Milestone 2)
    add_executable(test_scheduler
        tests/test_scheduler.cpp
    )
    target_link_libraries(test_scheduler PRIVATE kube32_core)
    add_test(NAME test_scheduler COMMAND test_scheduler)

    # 8. Autonomous Scheduler Adversarial Stress Tests (Milestone 2)
    add_executable(test_scheduler_adversarial
        tests/test_scheduler_adversarial.cpp
    )
    target_link_libraries(test_scheduler_adversarial PRIVATE kube32_core)
    add_test(NAME test_scheduler_adversarial COMMAND test_scheduler_adversarial)

    # 9. Hardware Abstraction Layer & Capability Firewall Tests (Milestone 3)
    add_executable(test_capability_firewall
        tests/test_capability_firewall.cpp
    )
    target_link_libraries(test_capability_firewall PRIVATE kube32_core)
    add_test(NAME test_capability_firewall COMMAND test_capability_firewall)

    # 10. Wasm3 Sandboxed Runtime & Trap Containment Tests (Milestone 3)
    add_executable(test_wasm_runtime
        tests/test_wasm_runtime.cpp
    )
    target_link_libraries(test_wasm_runtime PRIVATE kube32_core)
    add_test(NAME test_wasm_runtime COMMAND test_wasm_runtime)

    # 11. ESP32 Node Agent Kubelet & Reconciler Tests (Milestone 3)
    add_executable(test_kubelet
        tests/test_kubelet.cpp
    )
    target_link_libraries(test_kubelet PRIVATE kube32_core)
    add_test(NAME test_kubelet COMMAND test_kubelet)

    # 12. Milestone 3 Adversarial Sandboxing & Fault Containment Tests
    add_executable(test_m3_adversarial
        tests/test_m3_adversarial.cpp
    )
    target_link_libraries(test_m3_adversarial PRIVATE kube32_core)
    add_test(NAME test_m3_adversarial COMMAND test_m3_adversarial)

    # 13. HAL Capability Firewall & Peripheral Collision Adversarial Tests (Milestone 3)
    add_executable(test_hal_adversarial
        tests/test_hal_adversarial.cpp
    )
    target_link_libraries(test_hal_adversarial PRIVATE kube32_core)
    add_test(NAME test_hal_adversarial COMMAND test_hal_adversarial)

    # 14. Milestone 3 Iteration 2 Challenger 4 Adversarial Re-Verification Suite
    add_executable(test_m3_challenger_4
        tests/test_m3_challenger_4.cpp
    )
    target_link_libraries(test_m3_challenger_4 PRIVATE kube32_core)
    add_test(NAME test_m3_challenger_4 COMMAND test_m3_challenger_4)

endif()


