# ESP-NOW lives inside esp_wifi, it is not a component of its own.
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES cgusb cgproto cgbeacon cgoled
             esp_wifi esp_timer nvs_flash esp_netif esp_event
)
