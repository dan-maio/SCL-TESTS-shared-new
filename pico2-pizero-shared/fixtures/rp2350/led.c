/* led.c — RP2350 version of the WeAct libled: the board LED, through SIO. */
#include <stdint.h>

#define LED_BIT      (1u << 5)   /* Pi-Zero RP2350B: GPIO5 */
#define SIO_OUT      (*(volatile uint32_t*)0xD0000010u)
#define SIO_OUT_SET  (*(volatile uint32_t*)0xD0000018u)
#define SIO_OUT_CLR  (*(volatile uint32_t*)0xD0000020u)
#define SIO_OE_SET   (*(volatile uint32_t*)0xD0000038u)
#define GPIO5_CTRL   (*(volatile uint32_t*)(0x40028000u + 5u * 8u + 4u))
#define GPIO5_PAD    (*(volatile uint32_t*)(0x40038000u + 4u + 5u * 4u))

static int initialized = 0;
static uint32_t blinks = 0;

void led_init(void) {
    GPIO5_PAD &= ~(1u << 8);     /* clear pad isolation */
    GPIO5_CTRL = 5u;             /* function: SIO */
    SIO_OE_SET = LED_BIT;
    initialized = 1;
}

/* delay is a nop count, so the stress loop can pass 0. */
uint32_t led_blink(uint32_t count, uint32_t delay) {
    if (!initialized) return 0;
    for (uint32_t i = 0; i < count; i++) {
        SIO_OUT_SET = LED_BIT;
        for (volatile uint32_t d = 0; d < delay; d++) __asm__("nop");
        SIO_OUT_CLR = LED_BIT;
        for (volatile uint32_t d = 0; d < delay; d++) __asm__("nop");
        ++blinks;
    }
    return blinks;
}

uint32_t led_is_on(uint32_t a, uint32_t b) { (void)a; (void)b; return (SIO_OUT & LED_BIT) != 0; }
uint32_t led_count(uint32_t a, uint32_t b) { (void)a; (void)b; return blinks; }

void led_discard(void) {
    SIO_OUT_CLR = LED_BIT;
    initialized = 0;
}
