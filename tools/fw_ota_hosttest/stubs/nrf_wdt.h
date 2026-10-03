#ifndef STUB_NRF_WDT_H
#define STUB_NRF_WDT_H
#include <stdint.h>
#include <stdbool.h>
typedef struct { int unused; } sim_wdt_t;
extern sim_wdt_t g_sim_wdt;
#define NRF_WDT (&g_sim_wdt)
bool nrf_wdt_started(sim_wdt_t const *w);
void nrf_wdt_reload_request_set(sim_wdt_t *w, uint8_t rr);
#endif
