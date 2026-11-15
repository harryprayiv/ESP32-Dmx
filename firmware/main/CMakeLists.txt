idf_component_register(
    SRCS
        "main.cpp"
        "Esp32Dmx.cpp"
        "WebServer.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        esp_driver_gpio
        esp_driver_uart
        esp_event
        esp_http_server
        esp_netif
        esp_wifi
        nvs_flash
)

# The web console. index.html is embedded as it is. app.js is the bundle that
# `spago bundle` writes in web/, embedded gzipped.
set(web_dir "${CMAKE_CURRENT_LIST_DIR}/../../web")
set(app_js "${web_dir}/dist/app.js")
set(app_js_gz "${CMAKE_CURRENT_BINARY_DIR}/app.js.gz")

if(NOT CMAKE_BUILD_EARLY_EXPANSION AND NOT EXISTS "${app_js}")
    message(FATAL_ERROR "${app_js} is missing. Run `spago bundle` in web/ first.")
endif()

add_custom_command(
    OUTPUT "${app_js_gz}"
    COMMAND sh -c "gzip -9 -n -c '${app_js}' > '${app_js_gz}'"
    DEPENDS "${app_js}"
    VERBATIM
)
add_custom_target(web_console_bundle DEPENDS "${app_js_gz}")

target_add_binary_data(${COMPONENT_LIB} "${web_dir}/index.html" BINARY)
target_add_binary_data(${COMPONENT_LIB} "${app_js_gz}" BINARY DEPENDS web_console_bundle)