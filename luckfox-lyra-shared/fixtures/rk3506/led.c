/* led.c — RK3506 version of the WeAct libled: the board's user LED on GPIO1_A0. */
#include <stdint.h>

#define CRU_GATE22   (*(volatile uint32_t*)(0xFF9A0000u + 0x0858u))
#define CRU_GATE03   (*(volatile uint32_t*)(0xFF9A0000u + 0x080Cu))
#define CRU_RST22    (*(volatile uint32_t*)(0xFF9A0000u + 0x0A58u))
#define CRU_RST03    (*(volatile uint32_t*)(0xFF9A0000u + 0x0A0Cu))
#define IOMUX_A      (*(volatile uint32_t*)(0xFF660000u + 0x0020u))
#define GPIO1_DR     (*(volatile uint32_t*)(0xFF870000u + 0x0000u))
#define GPIO1_DDR    (*(volatile uint32_t*)(0xFF870000u + 0x0008u))
/* Rockchip write-mask registers: the upper 16 bits say which lower bits to change. */
#define WM(mask, val) (((uint32_t)(mask) << 16) | (uint32_t)(val))

static int initialized = 0;
static uint32_t blinks = 0;

void led_init(void) {
    CRU_GATE22 = WM(0x0002u, 0u);
    CRU_GATE03 = WM(0x0300u, 0u);
    CRU_RST22 = WM(0x0002u, 0u);
    CRU_RST03 = WM(0x0300u, 0u);
    IOMUX_A = WM(0x000Fu, 0u);
    GPIO1_DDR = WM(1u, 1u);
    initialized = 1;
}

/* delay is a nop count, so the stress loop can pass 0. */
uint32_t led_blink(uint32_t count, uint32_t delay) {
    if (!initialized) return 0;
    for (uint32_t i = 0; i < count; i++) {
        GPIO1_DR = WM(1u, 1u);
        for (volatile uint32_t d = 0; d < delay; d++) __asm__("nop");
        GPIO1_DR = WM(1u, 0u);
        for (volatile uint32_t d = 0; d < delay; d++) __asm__("nop");
        ++blinks;
    }
    return blinks;
}

uint32_t led_is_on(uint32_t a, uint32_t b) { (void)a; (void)b; return GPIO1_DR & 1u; }
uint32_t led_count(uint32_t a, uint32_t b) { (void)a; (void)b; return blinks; }

void led_discard(void) {
    GPIO1_DR = WM(1u, 0u);
    initialized = 0;
}
