/* Host-test stub: just enough of nrf.h for fw_ota_apply.c. */
#ifndef STUB_NRF_H
#define STUB_NRF_H
#include <stdint.h>
#include <stdbool.h>
typedef struct { volatile uint32_t GPREGRET; volatile uint32_t RESETREAS; } sim_power_t;
extern sim_power_t g_sim_power;
#define NRF_POWER (&g_sim_power)
#define POWER_RESETREAS_RESETPIN_Msk 0x1u
#define POWER_RESETREAS_SREQ_Msk     0x4u
void NVIC_SystemReset(void) __attribute__((noreturn));
#endif
