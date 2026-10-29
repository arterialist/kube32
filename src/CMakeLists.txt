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

set(KUBE32_APP_SRCS
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
    controller/node_lifecycle_controller.cpp
    controller/deployment_controller.cpp
    controller/workload_controller.cpp
    hal/capability_firewall.cpp
    hal/mock_hal.cpp
    hal/sd_card.cpp
    hal/node_storage.cpp
    hal/pod_buses.cpp
    hal/tft_keyboard_hardware.cpp
    ipc/channel.cpp
    kubelet/kube_api_client.cpp
    kubelet/kubelet.cpp
    kubelet/pod_env.cpp
    kubelet/metrics_provider.cpp
    kubelet/node_status_reporter.cpp
    mem/memory_tier.cpp
    mem/monotonic_arena.cpp
    models/kube_lease.cpp
    models/kube_models.cpp
    models/tolerations.cpp
    net/mdns_discovery.cpp
    net/network_manager.cpp
    osal/condition_variable_posix.cpp
    osal/mutex_posix.cpp
    osal/socket_posix.cpp
    osal/tls_mbedtls.cpp
    osal/system_posix.cpp
    osal/thread_posix.cpp
    osal/timer_posix.cpp
    runtime/container_lifecycle.cpp
    runtime/host_functions.cpp
    runtime/wasm_runtime.cpp
    runtime/app_pod.cpp
    runtime/pod_log.cpp
    runtime/image_registry.cpp
    runtime/pod_fs.cpp
    runtime/pod_memory_pool.cpp
    runtime/pod_assets.cpp
    app/ingress_controller.cpp
    app/ingress_routes.cpp
    app/pod_server.cpp
    app/service_client.cpp
    app/node_storage_api.cpp
    embedded/embedded_blobs_esp.cpp
    scheduler/scheduler.cpp
    storage/littlefs_journal.cpp
    storage/flash_log.cpp
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
        esp_lcd
        esp_adc
        esp_driver_spi
        esp_driver_sdspi
        sdmmc
        fatfs
        esp_partition
        mbedtls
)

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

# Container environment, log persistence and bus setup run once per container
# start or every few seconds: built for size, they take half the flash -O2 gives them.
set_source_files_properties(
    ${CMAKE_CURRENT_LIST_DIR}/kubelet/pod_env.cpp
    ${CMAKE_CURRENT_LIST_DIR}/runtime/pod_log.cpp
    ${CMAKE_CURRENT_LIST_DIR}/hal/pod_buses.cpp
    ${CMAKE_CURRENT_LIST_DIR}/app/node_storage_api.cpp
    ${CMAKE_CURRENT_LIST_DIR}/api/node_proxy.cpp
    ${CMAKE_CURRENT_LIST_DIR}/api/protobuf_body.cpp
    ${CMAKE_CURRENT_LIST_DIR}/app/service_client.cpp
    ${CMAKE_CURRENT_LIST_DIR}/runtime/pod_assets.cpp
    PROPERTIES COMPILE_OPTIONS "-Os")
