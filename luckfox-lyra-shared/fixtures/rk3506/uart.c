/* uart.c — RK3506 version of the WeAct libuart: UART1, already set up by the board. */
#include <stdint.h>

#define UART_THR   (*(volatile uint32_t*)0xFF0B0000u)
#define UART_LSR   (*(volatile uint32_t*)0xFF0B0014u)
#define LSR_THRE   (1u << 5)

static int initialized = 0;
static uint32_t chars = 0;

void uart_init(void) { initialized = 1; }

uint32_t uart_puts(const char* s) {
    if (!initialized) return 0;
    uint32_t n = 0;
    for (; *s; ++s, ++n) {
        while (!(UART_LSR & LSR_THRE)) {}
        UART_THR = (uint32_t)(uint8_t)*s;
    }
    chars += n;
    return n;
}

uint32_t uart_chars(uint32_t a, uint32_t b) { (void)a; (void)b; return chars; }

void uart_discard(void) { initialized = 0; }
