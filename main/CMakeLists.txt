idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp8266 esp_http_server nvs_flash tcpip_adapter)

