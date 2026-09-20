idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES cgproto cgoled
             esp_wifi esp_timer nvs_flash esp_netif esp_event driver
)
