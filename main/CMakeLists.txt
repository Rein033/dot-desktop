# The desktop (desk.c, gfx.c, the fonts, the shared frame's tiles and the software JPEG) is
# pure C; the Active Object and the stream bring the SDK and ESP-IDF.
idf_component_register(
    SRCS "app_main.c" "desk_ao.c" "desk.c" "gfx.c" "fonts_aa.c" "fbshare.c"
         "jpeg_enc.c" "sw_jpeg.c" "stream.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "web/index.html"
    REQUIRES kode
    PRIV_REQUIRES kdrv khal esp_hw_support esp_timer esp_system esp_http_server esp_driver_jpeg
)
