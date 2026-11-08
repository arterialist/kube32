# ==============================================================================
# Kube32 - ESP-IDF Component CMakeLists.txt
# ==============================================================================

set(WASM3_SRCS
    ${CMAKE_CURRENT_LIST_DIR}/../third_party/wasm3/source/m3_api_libc.c
    ${CMAKE_CURRENT_LIST_DIR}/../third_party/wasm3/source/m3_api_tracer.c
    ${CMAKE_CURRENT_LIST_DIR}/../third_party/wasm3/source/m3_bind.c
    ${CMAKE_CURRENT_LIST_DIR}/../third_party/wasm3/source/m3_code.c
    ${CMAKE_CURRENT_LIST_DIR}/../third_party/wasm3/source/m3_compile.c
    ${CMAKE_CURRENT_LIST_DIR}/../third_party/wasm3/source/m3_core.c
    ${CMAKE_CURRENT_LIST_DIR}/../third_party/wasm3/source/m3_deterministic.c
    ${CMAKE_CURRENT_LIST_DIR}/../third_party/wasm3/source/m3_env.c
    ${CMAKE_CURRENT_LIST_DIR}/../third_party/wasm3/source/m3_exec.c
    ${CMAKE_CURRENT_LIST_DIR}/../third_party/wasm3/source/m3_function.c
    ${CMAKE_CURRENT_LIST_DIR}/../third_party/wasm3/source/m3_info.c
    ${CMAKE_CURRENT_LIST_DIR}/../third_party/wasm3/source/m3_module.c
    ${CMAKE_CURRENT_LIST_DIR}/../third_party/wasm3/source/m3_parse.c
    ${CMAKE_CURRENT_LIST_DIR}/../third_party/wasm3/source/m3_snapshot.c
    ${CMAKE_CURRENT_LIST_DIR}/../third_party/wasm3/source/m3_validate.c
    ${CMAKE_CURRENT_LIST_DIR}/../third_party/wasm3/source/m3_xxh64.c
)

# The embedded image and bundle tables are generated at configure time, and
# again whenever an image changes (PlatformIO also refreshes them before each
# build, in tools/pio_embed.py).
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    idf_build_get_property(kube32_python PYTHON)
    set(kube32_root ${CMAKE_CURRENT_LIST_DIR}/..)
    execute_process(COMMAND ${kube32_python} ${kube32_root}/tools/embed_assets.py --esp
                    WORKING_DIRECTORY ${kube32_root} RESULT_VARIABLE kube32_embed_rc)
    if(NOT kube32_embed_rc EQUAL 0)
        message(FATAL_ERROR "tools/embed_assets.py failed")
    endif()
    file(GLOB_RECURSE kube32_image_files CONFIGURE_DEPENDS ${kube32_root}/images/*)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${kube32_root}/tools/embed_assets.py ${kube32_image_files})
endif()

set(KUBE32_APP_SRCS
    bench/bench_kernels.c
    api/http_server.cpp
    api/route_handler.cpp
    api/table_serializer.cpp
    api/selectors.cpp
    api/watch_hub.cpp
    api/event_recorder.cpp
    api/metrics_registry.cpp
    api/version_info.cpp
    api/timestamp_backfill.cpp
    api/node_proxy.cpp
    api/protobuf_body.cpp
    api/firmware_api.cpp
    api/checkpoint_api.cpp
    api/inline_images.cpp
    controller/node_lifecycle_controller.cpp
    controller/deployment_controller.cpp
    controller/workload_controller.cpp
    controller/firmware_rollout_controller.cpp
    controller/device_controller.cpp
    controller/checkpoint_controller.cpp
    controller/reconcile_trigger.cpp
    hal/capability_firewall.cpp
    hal/mock_hal.cpp
    hal/sd_card.cpp
    hal/node_storage.cpp
    hal/pod_buses.cpp
    hal/i2c_discovery.cpp
    hal/chip_temp.cpp
    hal/pod_rmt.cpp
    hal/tft_keyboard_hardware.cpp
    hal/board.cpp
    hal/firmware_slots.cpp
    ipc/channel.cpp
    kubelet/kube_api_client.cpp
    kubelet/kubelet.cpp
    kubelet/pod_env.cpp
    kubelet/metrics_provider.cpp
    kubelet/node_status_reporter.cpp
    kubelet/firmware_agent.cpp
    mem/large_blocks.cpp
    mem/memory_tier.cpp
    mem/monotonic_arena.cpp
    models/kube_lease.cpp
    models/kube_models.cpp
    models/json_patch.cpp
    models/tolerations.cpp
    models/device.cpp
    models/migration.cpp
    net/mdns_discovery.cpp
    net/network_manager.cpp
    net/network_policy.cpp
    osal/condition_variable_posix.cpp
    osal/mutex_posix.cpp
    osal/socket_posix.cpp
    osal/tls_mbedtls.cpp
    osal/system_posix.cpp
    osal/sha256.cpp
    osal/thread_posix.cpp
    osal/timer_posix.cpp
    runtime/container_lifecycle.cpp
    runtime/host_functions.cpp
    runtime/wasm_runtime.cpp
    runtime/app_pod.cpp
    runtime/pod_linalg.cpp
    runtime/pod_raster.cpp
    runtime/pod_blobs.cpp
    runtime/pod_log.cpp
    runtime/image_registry.cpp
    runtime/pod_fs.cpp
    runtime/pod_memory_pool.cpp
    runtime/pod_assets.cpp
    runtime/pod_snapshot.cpp
    runtime/app_pod_migration.cpp
    app/ingress_controller.cpp
    app/ingress_routes.cpp
    app/pod_server.cpp
    app/service_client.cpp
    app/egress_policy.cpp
    app/egress_gateway.cpp
    app/node_storage_api.cpp
    app/node_blobs_api.cpp
    app/pod_checkpoint_api.cpp
    app/console_relay.cpp
    app/improv_serial.cpp
    embedded/embedded_blobs_esp.cpp
    scheduler/scheduler.cpp
    storage/littlefs_journal.cpp
    storage/flash_log.cpp
    storage/blob_store.cpp
    storage/persist_policy.cpp
    storage/radix_tree.cpp
    storage/store.cpp
    main_esp32.cpp
)

idf_component_register(
    SRCS ${KUBE32_APP_SRCS} ${WASM3_SRCS}
    INCLUDE_DIRS 
        "${CMAKE_CURRENT_LIST_DIR}/../include"
        "${CMAKE_CURRENT_LIST_DIR}/../third_party/wasm3/source"
    REQUIRES
        freertos
        esp_timer
        esp_wifi
        nvs_flash
        esp_event
        esp_netif
        esp_hw_support
        lwip
        vfs
        driver
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_i2s
        esp_driver_ledc
        esp_driver_rmt
        esp_driver_tsens
        esp_driver_uart
        esp_driver_usb_serial_jtag
        esp_lcd
        esp_adc
        esp_driver_spi
        esp_driver_sdspi
        sdmmc
        fatfs
        esp_partition
        mbedtls
        app_update
        bootloader_support
)

# What the firmware needs from every build system: PlatformIO adds the same
# from platformio.ini, a plain idf.py build gets them from here.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    KUBE32_TARGET_ESP32=1
    d_m3CodePageAlignSize=4096
    d_m3HasExceptionHandling=0
    d_m3HasTypedRefs=0
    d_m3HasStackSwitching=0
    d_m3HasSnapshots=0
    d_m3HasGasMetering=1
    d_m3MaxFunctionStackHeight=1024
    d_m3RecordBacktraces=0
    d_m3ParseNameSection=0
    d_m3MaxDuplicateFunctionImpl=1
    d_m3MaxNativeStack=98304
    d_m3NativeStackMargin=6144
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format -Wno-missing-field-initializers)

# Control plane, object store and kubelet code runs on requests and changes,
# not per Wasm instruction: compile it for size. The interpreter, runtime,
# HAL and IPC keep -O2.
set(KUBE32_SIZE_SRCS ${KUBE32_APP_SRCS})
list(FILTER KUBE32_SIZE_SRCS INCLUDE REGEX "^(api|controller|scheduler|storage|kubelet)/|^main_esp32\\.cpp$")
# Every request parses and writes JSON and Tables: keep those at -O2.
list(FILTER KUBE32_SIZE_SRCS EXCLUDE REGEX "^api/(http_server|table_serializer|protobuf_body)\\.cpp$")
set_source_files_properties(${KUBE32_SIZE_SRCS} PROPERTIES COMPILE_OPTIONS "-Os")
# The ESP32-S3 loads and stores at any address in hardware: a Wasm memory
# access is one instruction instead of a byte-wise copy
set_source_files_properties(${WASM3_SRCS} PROPERTIES COMPILE_OPTIONS "-mno-strict-align")
# The pods' linear algebra computes the bits kube32-sim computes: no fused
# multiply-adds (the LX7 FPU has madd.s)
set_source_files_properties(runtime/pod_linalg.cpp PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")

# Build identity for GET /version (commit, tree state, build time); see
# cmake/kube32_build_info.cmake. Regenerated when a source file or the git
# checkout changes.
set(KUBE32_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)
set(KUBE32_BUILD_INFO_H ${CMAKE_CURRENT_BINARY_DIR}/kube32_build_info.h)
file(GLOB_RECURSE KUBE32_BUILD_INFO_INPUTS CONFIGURE_DEPENDS
     ${KUBE32_ROOT}/src/*.cpp ${KUBE32_ROOT}/src/*.c ${KUBE32_ROOT}/include/*.hpp ${KUBE32_ROOT}/include/*.h)
foreach(git_file HEAD index logs/HEAD)
    if(EXISTS ${KUBE32_ROOT}/.git/${git_file})
        list(APPEND KUBE32_BUILD_INFO_INPUTS ${KUBE32_ROOT}/.git/${git_file})
    endif()
endforeach()
add_custom_command(
    OUTPUT ${KUBE32_BUILD_INFO_H}
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${KUBE32_ROOT} -DOUTPUT=${KUBE32_BUILD_INFO_H}
            -P ${KUBE32_ROOT}/cmake/kube32_build_info.cmake
    DEPENDS ${KUBE32_BUILD_INFO_INPUTS} ${KUBE32_ROOT}/cmake/kube32_build_info.cmake
    COMMENT "Recording build info"
)
add_custom_target(kube32_build_info DEPENDS ${KUBE32_BUILD_INFO_H})
add_dependencies(${COMPONENT_LIB} kube32_build_info)
target_include_directories(${COMPONENT_LIB} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/api/version_info.cpp PROPERTIES OBJECT_DEPENDS ${KUBE32_BUILD_INFO_H})

# Container environment, log persistence, bus setup and IR channels run once
# per container start, every few seconds or once per IR frame, firmware
# rollouts once per update, the Device controller and I2C discovery every
# few seconds, the kubelet's passes and the module loader once a second or
# once per container start, a module's bytes once per container start,
# snapshots and Checkpoint data once per move,
# Improv packets during a browser install:
# built for size, they take half the flash -O2 gives them.
set_source_files_properties(
    ${CMAKE_CURRENT_LIST_DIR}/kubelet/pod_env.cpp
    ${CMAKE_CURRENT_LIST_DIR}/runtime/pod_log.cpp
    ${CMAKE_CURRENT_LIST_DIR}/hal/pod_buses.cpp
    ${CMAKE_CURRENT_LIST_DIR}/hal/pod_rmt.cpp
    ${CMAKE_CURRENT_LIST_DIR}/app/node_storage_api.cpp
    ${CMAKE_CURRENT_LIST_DIR}/app/node_blobs_api.cpp
    ${CMAKE_CURRENT_LIST_DIR}/storage/blob_store.cpp
    ${CMAKE_CURRENT_LIST_DIR}/api/node_proxy.cpp
    ${CMAKE_CURRENT_LIST_DIR}/api/protobuf_body.cpp
    ${CMAKE_CURRENT_LIST_DIR}/app/service_client.cpp
    ${CMAKE_CURRENT_LIST_DIR}/app/egress_policy.cpp
    ${CMAKE_CURRENT_LIST_DIR}/net/network_policy.cpp
    ${CMAKE_CURRENT_LIST_DIR}/runtime/pod_assets.cpp
    ${CMAKE_CURRENT_LIST_DIR}/api/firmware_api.cpp
    ${CMAKE_CURRENT_LIST_DIR}/controller/firmware_rollout_controller.cpp
    ${CMAKE_CURRENT_LIST_DIR}/hal/firmware_slots.cpp
    ${CMAKE_CURRENT_LIST_DIR}/kubelet/firmware_agent.cpp
    ${CMAKE_CURRENT_LIST_DIR}/controller/device_controller.cpp
    ${CMAKE_CURRENT_LIST_DIR}/controller/checkpoint_controller.cpp
    ${CMAKE_CURRENT_LIST_DIR}/models/migration.cpp
    ${CMAKE_CURRENT_LIST_DIR}/models/device.cpp
    ${CMAKE_CURRENT_LIST_DIR}/hal/i2c_discovery.cpp
    ${CMAKE_CURRENT_LIST_DIR}/runtime/pod_snapshot.cpp
    ${CMAKE_CURRENT_LIST_DIR}/runtime/app_pod_migration.cpp
    ${CMAKE_CURRENT_LIST_DIR}/app/pod_checkpoint_api.cpp
    ${CMAKE_CURRENT_LIST_DIR}/app/improv_serial.cpp
    ${CMAKE_CURRENT_LIST_DIR}/api/checkpoint_api.cpp
    ${CMAKE_CURRENT_LIST_DIR}/kubelet/kubelet.cpp
    ${CMAKE_CURRENT_LIST_DIR}/runtime/wasm_runtime.cpp
    ${CMAKE_CURRENT_LIST_DIR}/runtime/image_registry.cpp
    PROPERTIES COMPILE_OPTIONS "-Os")
