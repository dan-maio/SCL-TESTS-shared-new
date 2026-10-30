/* spi.c — RP2350 version of the WeAct libspi. This board has no W25Q64, so SPI0 runs in the
 * PL022's internal loopback: what it sends comes straight back, with no wiring. */
#include <stdint.h>

#define RESETS_CLR     (*(volatile uint32_t*)(0x40020000u + 0x3000u))
#define RESETS_DONE    (*(volatile uint32_t*)0x40020008u)
#define RESET_SPI0     (1u << 18)
#define SSP_CR0        (*(volatile uint32_t*)0x40080000u)
#define SSP_CR1        (*(volatile uint32_t*)0x40080004u)
#define SSP_DR         (*(volatile uint32_t*)0x40080008u)
#define SSP_SR         (*(volatile uint32_t*)0x4008000Cu)
#define SSP_CPSR       (*(volatile uint32_t*)0x40080010u)
#define SR_TNF         (1u << 1)
#define SR_RNE         (1u << 2)
#define CR1_LBM        (1u << 0)
#define CR1_SSE        (1u << 1)

static int initialized = 0;
static uint32_t transfers = 0;

void spi_init(void) {
    RESETS_CLR = RESET_SPI0;
    while (!(RESETS_DONE & RESET_SPI0)) {}
    SSP_CPSR = 2u;
    SSP_CR0 = 7u;                /* 8-bit frames */
    SSP_CR1 = CR1_LBM;
    SSP_CR1 = CR1_LBM | CR1_SSE;
    while (SSP_SR & SR_RNE) (void)SSP_DR;
    initialized = 1;
}

static uint8_t xfer(uint8_t d) {
    while (!(SSP_SR & SR_TNF)) {}
    SSP_DR = d;
    while (!(SSP_SR & SR_RNE)) {}
    return (uint8_t)SSP_DR;
}

/* Send a pattern from seed and return how many bytes came back different. */
uint32_t spi_selftest(uint32_t seed, uint32_t len) {
    if (!initialized) return len;
    uint32_t bad = 0;
    uint8_t v = (uint8_t)seed;
    for (uint32_t i = 0; i < len; i++, v = (uint8_t)(v * 31u + 7u)) {
        if (xfer(v) != v) ++bad;
        ++transfers;
    }
    return bad;
}

uint32_t spi_transfers(uint32_t a, uint32_t b) { (void)a; (void)b; return transfers; }

void spi_discard(void) {
    SSP_CR1 = CR1_LBM;
    initialized = 0;
}
