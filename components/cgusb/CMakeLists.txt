# cgusb.c is plain C and is also built by test/host under MinGW (D-007).
# cgusb_link.c is the ESP-IDF transport and stays out of the host build.
idf_component_register(
    SRCS "cgusb.c" "cgusb_link.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_common
)
