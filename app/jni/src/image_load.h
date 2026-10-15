#pragma once

#include <cstdint>
#include <string>

// Load a JPEG/PNG image from disk and decode to RGBA8.
// On success:
//   *out_pixels is a malloc'd buffer of *out_w * *out_h * 4 bytes
//   *out_w and *out_h are set
//   Caller owns *out_pixels and must free() it.
// Returns false on failure.
bool load_image_rgba(const char* path,
                     int* out_w, int* out_h,
                     uint8_t** out_pixels);
