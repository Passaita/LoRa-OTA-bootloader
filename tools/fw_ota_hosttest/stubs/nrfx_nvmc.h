#ifndef STUB_NRFX_NVMC_H
#define STUB_NRFX_NVMC_H
#include <stdint.h>
void nrfx_nvmc_page_erase(uint32_t addr);
void nrfx_nvmc_words_write(uint32_t addr, void const *src, uint32_t num_words);
#endif
