idf_component_register(
    SRCS "main.c" "flight_comm.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_netif esp_wifi esp_event espnow driver
)