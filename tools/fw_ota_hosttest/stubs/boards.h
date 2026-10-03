#ifndef STUB_BOARDS_H
#define STUB_BOARDS_H
#include <stdint.h>
#include <stdbool.h>
#define PRINTF(...) do {} while (0)
#define BUTTON_DFU 1
bool button_pressed(uint32_t pin);
#endif
