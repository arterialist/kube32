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
    target_compile_definitions(wasm3 PUBLIC d_m3CodePageAlignSize=4096 d_m3MaxFunctionStackHeight=1024 d_m3HasExceptionHandling=0 d_m3HasTypedRefs=0 d_m3HasStackSwitching=0 d_m3HasSnapshots=0 d_m3RecordBacktraces=0)

    # Core Library: OSAL, Memory Architecture, Storage Engine, API Server, Scheduler, HAL, Runtime, Kubelet
    set(KUBE32_CORE_SRCS
        src/osal/thread_posix.cpp
        src/osal/mutex_posix.cpp
        src/osal/condition_variable_posix.cpp
        src/osal/timer_posix.cpp
        src/osal/socket_posix.cpp
        src/osal/tls_mbedtls.cpp
        src/osal/tls_openssl.cpp
        src/osal/system_posix.cpp
        src/mem/memory_tier.cpp
        src/mem/monotonic_arena.cpp
        src/storage/radix_tree.cpp
        src/storage/littlefs_journal.cpp
        src/storage/flash_log.cpp
        src/storage/persist_policy.cpp
        src/storage/store.cpp
        src/models/kube_models.cpp
        src/models/kube_lease.cpp
        src/models/tolerations.cpp
        src/net/network_manager.cpp
        src/net/mdns_discovery.cpp
        src/net/network_policy.cpp
        src/controller/node_lifecycle_controller.cpp
        src/controller/deployment_controller.cpp
        src/controller/workload_controller.cpp
        src/ipc/channel.cpp
        src/api/table_serializer.cpp
        src/api/selectors.cpp
        src/api/watch_hub.cpp
        src/api/event_recorder.cpp
        src/api/metrics_registry.cpp
        src/api/route_handler.cpp
        src/api/http_server.cpp
        src/api/version_info.cpp
        src/api/timestamp_backfill.cpp
        src/api/node_proxy.cpp
        src/api/protobuf_body.cpp
        src/scheduler/scheduler.cpp
        src/hal/capability_firewall.cpp
        src/hal/sd_card.cpp
        src/hal/node_storage.cpp
        src/hal/pod_buses.cpp
        src/hal/pod_rmt.cpp
        src/hal/mock_hal.cpp
        src/runtime/host_functions.cpp
        src/runtime/container_lifecycle.cpp
        src/runtime/wasm_runtime.cpp
        src/runtime/app_pod.cpp
        src/runtime/pod_log.cpp
        src/runtime/image_registry.cpp
        src/runtime/pod_fs.cpp
        src/runtime/pod_memory_pool.cpp
        src/runtime/pod_assets.cpp
        src/app/ingress_controller.cpp
        src/app/ingress_routes.cpp
        src/app/pod_server.cpp
        src/app/service_client.cpp
        src/app/node_storage_api.cpp
        src/app/console_relay.cpp
        src/kubelet/node_status_reporter.cpp
        src/kubelet/metrics_provider.cpp
        src/kubelet/kube_api_client.cpp
        src/kubelet/kubelet.cpp
        src/kubelet/pod_env.cpp
        ${CMAKE_BINARY_DIR}/generated/embedded_blobs.cpp
    )

    # Embedded images (images/*.wasm) and static bundles (images/web/<bundle>/)
    # are turned into a generated source so the host simulator serves the same
    # bytes as the firmware.
    file(GLOB KUBE32_EMBED_INPUTS ${CMAKE_CURRENT_SOURCE_DIR}/images/*.wasm)
    file(GLOB_RECURSE KUBE32_EMBED_BUNDLE_INPUTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/images/web/*)
    list(APPEND KUBE32_EMBED_INPUTS ${KUBE32_EMBED_BUNDLE_INPUTS})
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/generated/embedded_blobs.cpp
        COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_assets.py --host-out ${CMAKE_BINARY_DIR}/generated
        DEPENDS ${KUBE32_EMBED_INPUTS} ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_assets.py
        COMMENT "Embedding Wasm images and web assets"
    )

    # Build identity for GET /version (commit, tree state, build time). The
    # header is regenerated when a source file or the git checkout changes.
    set(KUBE32_BUILD_INFO_H ${CMAKE_BINARY_DIR}/generated/kube32_build_info.h)
    file(GLOB_RECURSE KUBE32_BUILD_INFO_INPUTS CONFIGURE_DEPENDS
         ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c
         ${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h)
    foreach(git_file HEAD index logs/HEAD)
        if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/.git/${git_file})
            list(APPEND KUBE32_BUILD_INFO_INPUTS ${CMAKE_CURRENT_SOURCE_DIR}/.git/${git_file})
        endif()
    endforeach()
    add_custom_command(
        OUTPUT ${KUBE32_BUILD_INFO_H}
        COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR} -DOUTPUT=${KUBE32_BUILD_INFO_H}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/kube32_build_info.cmake
        DEPENDS ${KUBE32_BUILD_INFO_INPUTS} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/kube32_build_info.cmake
        COMMENT "Recording build info"
    )

    add_library(kube32_core STATIC
        ${KUBE32_CORE_SRCS}
        ${KUBE32_BUILD_INFO_H}
    )
    target_include_directories(kube32_core PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/wasm3/source
    )
    target_include_directories(kube32_core PRIVATE ${CMAKE_BINARY_DIR}/generated)
    target_link_libraries(kube32_core PUBLIC wasm3 pthread)

    # TLS on the simulator's API port (--tls): OpenSSL when the host has it
    # (Homebrew keeps it outside the default search path)
    if(APPLE AND NOT OPENSSL_ROOT_DIR)
        foreach(prefix /opt/homebrew/opt/openssl@3 /usr/local/opt/openssl@3)
            if(EXISTS ${prefix}/include/openssl/ssl.h)
                set(OPENSSL_ROOT_DIR ${prefix})
            endif()
        endforeach()
    endif()
    find_package(OpenSSL 3 QUIET)
    if(OpenSSL_FOUND)
        target_compile_definitions(kube32_core PUBLIC KUBE32_HAS_OPENSSL=1)
        target_link_libraries(kube32_core PUBLIC OpenSSL::SSL OpenSSL::Crypto)
        message(STATUS "[Kube32] Simulator TLS: OpenSSL ${OPENSSL_VERSION}")
    else()
        message(STATUS "[Kube32] Simulator TLS: off (OpenSSL 3 not found)")
    endif()

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

    # 3. Memory architecture edge cases, concurrency and overflow defenses
    add_executable(test_memory_edge_cases
        tests/test_memory_edge_cases.cpp
    )
    target_link_libraries(test_memory_edge_cases PRIVATE kube32_core)
    add_test(NAME test_memory_edge_cases COMMAND test_memory_edge_cases)

    # 4. Storage Engine Baseline Tests
    add_executable(test_storage
        tests/test_storage.cpp
    )
    target_link_libraries(test_storage PRIVATE kube32_core)
    add_test(NAME test_storage COMMAND test_storage)

    # 5. Storage engine edge cases, CAS conflicts and crash recovery
    add_executable(test_storage_edge_cases
        tests/test_storage_edge_cases.cpp
    )
    target_link_libraries(test_storage_edge_cases PRIVATE kube32_core)
    add_test(NAME test_storage_edge_cases COMMAND test_storage_edge_cases)

    # 5b. Object store on raw flash: log-structured journal, power cuts, reclamation, write coalescing
    add_executable(test_store_flash_journal
        tests/test_store_flash_journal.cpp
    )
    target_link_libraries(test_store_flash_journal PRIVATE kube32_core)
    add_test(NAME test_store_flash_journal COMMAND test_store_flash_journal)

    # 6. Kubernetes API server and route handler
    add_executable(test_api_server
        tests/test_api_server.cpp
    )
    target_link_libraries(test_api_server PRIVATE kube32_core)
    add_test(NAME test_api_server COMMAND test_api_server)

    # 6b. Watch streams, pod log rings, events and metrics.k8s.io
    add_executable(test_api_streams
        tests/test_api_streams.cpp
    )
    target_link_libraries(test_api_streams PRIVATE kube32_core)
    add_test(NAME test_api_streams COMMAND test_api_streams)

    # 6c. Authentication, dry run, pagination, patch types, Endpoints, generation, Deployment conditions
    add_executable(test_api_control_plane
        tests/test_api_control_plane.cpp
    )
    target_link_libraries(test_api_control_plane PRIVATE kube32_core)
    add_test(NAME test_api_control_plane COMMAND test_api_control_plane)

    # 7. Scheduler and predicates
    add_executable(test_scheduler
        tests/test_scheduler.cpp
    )
    target_link_libraries(test_scheduler PRIVATE kube32_core)
    add_test(NAME test_scheduler COMMAND test_scheduler)

    # 8. Scheduler quantity parsing, capacity boundaries and OCC binding
    add_executable(test_scheduler_edge_cases
        tests/test_scheduler_edge_cases.cpp
    )
    target_link_libraries(test_scheduler_edge_cases PRIVATE kube32_core)
    add_test(NAME test_scheduler_edge_cases COMMAND test_scheduler_edge_cases)

    # 9. Hardware abstraction layer and capability firewall
    add_executable(test_capability_firewall
        tests/test_capability_firewall.cpp
    )
    target_link_libraries(test_capability_firewall PRIVATE kube32_core)
    add_test(NAME test_capability_firewall COMMAND test_capability_firewall)

    # 10. Wasm3 sandboxed runtime and trap containment
    add_executable(test_wasm_runtime
        tests/test_wasm_runtime.cpp
    )
    target_link_libraries(test_wasm_runtime PRIVATE kube32_core)
    add_test(NAME test_wasm_runtime COMMAND test_wasm_runtime)

    # 11. Kubelet node agent and reconciler
    add_executable(test_kubelet
        tests/test_kubelet.cpp
    )
    target_link_libraries(test_kubelet PRIVATE kube32_core)
    add_test(NAME test_kubelet COMMAND test_kubelet)

    # Typed-client protobuf request bodies
    add_executable(test_protobuf_body tests/test_protobuf_body.cpp)
    target_link_libraries(test_protobuf_body PRIVATE kube32_core)
    add_test(NAME test_protobuf_body COMMAND test_protobuf_body)

    # Service calls, bundle files, sub-page memory limits, the kubelet's
    # streamed API reads and node storage over HTTP (pods: apps/svcprobe)
    foreach(t test_service_calls test_pod_assets test_pod_memory_caps test_kubelet_api_stream test_node_storage)
        add_executable(${t} tests/${t}.cpp)
        target_link_libraries(${t} PRIVATE kube32_core)
        target_compile_definitions(${t} PRIVATE KUBE32_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
        add_test(NAME ${t} COMMAND ${t})
    endforeach()

    # 12. Container fault containment, memory quotas and restart back-off
    add_executable(test_container_faults
        tests/test_container_faults.cpp
    )
    target_link_libraries(test_container_faults PRIVATE kube32_core)
    add_test(NAME test_container_faults COMMAND test_container_faults)

    # 13. HAL peripheral access control, collisions and sensor emulation from wasm
    add_executable(test_hal_peripherals
        tests/test_hal_peripherals.cpp
    )
    target_link_libraries(test_hal_peripherals PRIVATE kube32_core)
    add_test(NAME test_hal_peripherals COMMAND test_hal_peripherals)

    # 14. Container termination of non-yielding guests and pin rollback
    add_executable(test_container_termination
        tests/test_container_termination.cpp
    )
    target_link_libraries(test_container_termination PRIVATE kube32_core)
    add_test(NAME test_container_termination COMMAND test_container_termination)

    # 15. Container stop grace periods, destruction and pin rollback contention
    add_executable(test_container_grace_period
        tests/test_container_grace_period.cpp
    )
    target_link_libraries(test_container_grace_period PRIVATE kube32_core)
    add_test(NAME test_container_grace_period COMMAND test_container_grace_period)

    # 16. HTTP chunked-body decoder fuzzing
    add_executable(test_chunked_decoder
        tests/test_chunked_decoder.cpp
    )
    target_link_libraries(test_chunked_decoder PRIVATE kube32_core)
    add_test(NAME test_chunked_decoder COMMAND test_chunked_decoder)

    # 17. Thread name lengths and concurrency
    add_executable(test_thread_names
        tests/test_thread_names.cpp
    )
    target_link_libraries(test_thread_names PRIVATE kube32_core)
    add_test(NAME test_thread_names COMMAND test_thread_names)

    # 18. Virtual channel IPC
    add_executable(test_channel_ipc
        tests/test_channel_ipc.cpp
    )
    target_link_libraries(test_channel_ipc PRIVATE kube32_core)
    add_test(NAME test_channel_ipc COMMAND test_channel_ipc)

    # 19. Node lease API and node lifecycle controller
    add_executable(test_lease_controller
        tests/test_lease_controller.cpp
    )
    target_link_libraries(test_lease_controller PRIVATE kube32_core)
    add_test(NAME test_lease_controller COMMAND test_lease_controller)
    # macOS libc fills freed blocks, so reads through stale JSON pointers fail
    set_tests_properties(test_lease_controller PROPERTIES ENVIRONMENT "MallocScribble=1")

    # 20. mDNS service discovery
    add_executable(test_mdns_discovery
        tests/test_mdns_discovery.cpp
    )
    target_link_libraries(test_mdns_discovery PRIVATE kube32_core)
    add_test(NAME test_mdns_discovery COMMAND test_mdns_discovery)

    # 21. Channel IPC concurrency, wire packet corruption and wasm sandbox boundaries
    add_executable(test_channel_ipc_stress
        tests/test_channel_ipc_stress.cpp
    )
    target_link_libraries(test_channel_ipc_stress PRIVATE kube32_core)
    add_test(NAME test_channel_ipc_stress COMMAND test_channel_ipc_stress)

    # 22. microSD card: HAL over KUBE32_SD_ROOT, pod storage ABI, images pulled from the card
    add_executable(test_sd_card
        tests/test_sd_card.cpp
    )
    target_link_libraries(test_sd_card PRIVATE kube32_core)
    target_compile_definitions(test_sd_card PRIVATE KUBE32_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME test_sd_card COMMAND test_sd_card)

    # 23. Ingress controller: networking.k8s.io/v1 routing, service resolution, static bundles, event streams
    add_executable(test_ingress
        tests/test_ingress.cpp
    )
    target_link_libraries(test_ingress PRIVATE kube32_core)
    add_test(NAME test_ingress COMMAND test_ingress)

    # 24. Kubelet node status: hardware pin admission, trap exit codes, node telemetry
    add_executable(test_kubelet_node_status
        tests/test_kubelet_node_status.cpp
    )
    target_link_libraries(test_kubelet_node_status PRIVATE kube32_core)
    add_test(NAME test_kubelet_node_status COMMAND test_kubelet_node_status)

    # 25. Pipeline memory pooling, K32C wire fuzzing, watchdog and yield behaviour
    add_executable(test_ipc_pipeline
        tests/test_ipc_pipeline.cpp
    )
    target_link_libraries(test_ipc_pipeline PRIVATE kube32_core)
    add_test(NAME test_ipc_pipeline COMMAND test_ipc_pipeline)

    # Container environment: service links, ConfigMap/Secret references, CreateContainerConfigError
    add_executable(test_container_env
        tests/test_container_env.cpp
    )
    target_link_libraries(test_container_env PRIVATE kube32_core)
    add_test(NAME test_container_env COMMAND test_container_env)

    # I2C and UART for pods: admission, driver lifecycle, node capacity, the i2cscan example
    add_executable(test_pod_buses
        tests/test_pod_buses.cpp
    )
    target_link_libraries(test_pod_buses PRIVATE kube32_core)
    add_test(NAME test_pod_buses COMMAND test_pod_buses)

    # RMT (IR) for pods: admission, channel lifecycle, receive queue, node capacity, the irremote example
    add_executable(test_pod_rmt
        tests/test_pod_rmt.cpp
    )
    target_link_libraries(test_pod_rmt PRIVATE kube32_core)
    add_test(NAME test_pod_rmt COMMAND test_pod_rmt)

    # JSON merge patch and strategic merge patch
    add_executable(test_patch
        tests/test_patch.cpp
    )
    target_link_libraries(test_patch PRIVATE kube32_core)
    add_test(NAME test_patch COMMAND test_patch)

    # HTTP streams over the serial console
    add_executable(test_console_relay
        tests/test_console_relay.cpp
    )
    target_link_libraries(test_console_relay PRIVATE kube32_core)
    add_test(NAME test_console_relay COMMAND test_console_relay)

endif()



