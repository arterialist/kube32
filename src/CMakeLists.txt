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
    controller/node_lifecycle_controller.cpp
    hal/capability_firewall.cpp
    hal/mock_hal.cpp
    hal/tft_keyboard_hardware.cpp
    ipc/channel.cpp
    kubelet/kube_api_client.cpp
    kubelet/kubelet.cpp
    kubelet/node_status_reporter.cpp
    mem/memory_tier.cpp
    mem/monotonic_arena.cpp
    models/kube_lease.cpp
    models/kube_models.cpp
    net/mdns_discovery.cpp
    net/network_manager.cpp
    osal/condition_variable_posix.cpp
    osal/mutex_posix.cpp
    osal/socket_posix.cpp
    osal/system_posix.cpp
    osal/thread_posix.cpp
    osal/timer_posix.cpp
    runtime/container_lifecycle.cpp
    runtime/host_functions.cpp
    runtime/wasm_runtime.cpp
    scheduler/scheduler.cpp
    storage/littlefs_journal.cpp
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
)
