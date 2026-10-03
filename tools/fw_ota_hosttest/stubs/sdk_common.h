#ifndef STUB_SDK_COMMON_H
#define STUB_SDK_COMMON_H
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#define NRF_MODULE_ENABLED(x) 1   /* the real crc16.c is gated on the SDK config; the bootloader build enables it */
#endif
