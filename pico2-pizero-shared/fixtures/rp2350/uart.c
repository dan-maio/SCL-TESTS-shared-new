/* uart.c — RP2350 version of the WeAct libuart: UART0, already set up by the board. */
#include <stdint.h>

#define UART_DR    (*(volatile uint32_t*)0x40070000u)
#define UART_FR    (*(volatile uint32_t*)0x40070018u)
#define FR_TXFF    (1u << 5)
#define UART_LOCK  (*(volatile uint32_t*)(0xD0000100u + 4u))  /* the board's console lock */

static int initialized = 0;
static uint32_t chars = 0;

void uart_init(void) { initialized = 1; }

uint32_t uart_puts(const char* s) {
    if (!initialized) return 0;
    uint32_t n = 0;
    while (UART_LOCK == 0) {}
    for (; *s; ++s, ++n) {
        while (UART_FR & FR_TXFF) {}
        UART_DR = (uint32_t)(uint8_t)*s;
    }
    UART_LOCK = 1;
    chars += n;
    return n;
}

uint32_t uart_chars(uint32_t a, uint32_t b) { (void)a; (void)b; return chars; }

void uart_discard(void) { initialized = 0; }
