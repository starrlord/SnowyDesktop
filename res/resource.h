// Resource identifiers for snowystarr.rc
#pragma once

#define IDI_APP             101
#define IDI_APP_PAUSED      102

// Classic pixel-art flake bitmaps (PNG, RCDATA), consecutive ids.
#define IDR_CLASSIC_FIRST   201
#define IDR_CLASSIC1        201
#define IDR_CLASSIC2        202
#define IDR_CLASSIC3        203
#define IDR_CLASSIC4        204
#define IDR_CLASSIC5        205
#define IDR_CLASSIC6        206
#define IDR_CLASSIC7        207
#define IDR_CLASSIC8        208

// The version comes from CMake (project VERSION, overridden by CI from the
// release tag); these defaults only apply to builds outside CMake.
#ifndef SNOWYSTARR_VERSION_MAJOR
#define SNOWYSTARR_VERSION_MAJOR 0
#define SNOWYSTARR_VERSION_MINOR 0
#define SNOWYSTARR_VERSION_PATCH 0
#define SNOWYSTARR_VERSION_STR   "0.0.0-dev"
#endif
