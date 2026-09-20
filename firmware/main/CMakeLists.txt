idf_component_register(SRCS "app_main.c" INCLUDE_DIRS "."
    REQUIRES zt_contract zt_hal zt_ui zt_radio zt_game zt_store zt_gateway zt_console zt_demo zt_ops
    PRIV_REQUIRES log freertos mbedtls esp_system esp_netif)

# Credentials are local build inputs; fail clearly instead of compiling a dummy token.
set(ZT_PRIVATE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../private")
if(NOT EXISTS "${ZT_PRIVATE_DIR}/zt_gateway_private.h")
    message(FATAL_ERROR "Missing private/zt_gateway_private.h (ZT_PRIVATE_GATEWAY_TOKEN)")
endif()
if(NOT EXISTS "${ZT_PRIVATE_DIR}/zt_device_private.h")
    message(FATAL_ERROR "Missing private/zt_device_private.h (Wi-Fi and mesh settings; see docs/device-private.example.h)")
endif()
target_include_directories(${COMPONENT_LIB} PRIVATE "${ZT_PRIVATE_DIR}")
