#ifndef UART_H
#define UART_H
#ifdef __cplusplus
extern "C" {
#endif
void uart_init(void);
void uart_puts(const char* s);   /* writes s then CRLF on USART1 (PA9, 115200 8N1) */
void uart_discard(void);
#ifdef __cplusplus
}
#endif
#endif
