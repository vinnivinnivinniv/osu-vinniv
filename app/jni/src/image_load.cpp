#include "image_load.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

bool load_image_rgba(const char* path,
                     int* out_w, int* out_h,
                     uint8_t** out_pixels) {
    *out_w = 0;
    *out_h = 0;
    *out_pixels = nullptr;

    int w = 0, h = 0, channels = 0;
    // Request 4 channels so we always get RGBA regardless of source format
    uint8_t* data = stbi_load(path, &w, &h, &channels, 4);
    if (!data) return false;

    *out_w = w;
    *out_h = h;
    *out_pixels = data;
    return true;
}
