# cgproto uses cgusb_crc16 so there is one checksum in the system (D-005).
idf_component_register(
    SRCS "cgproto.c"
    INCLUDE_DIRS "include"
    REQUIRES cgusb
)
