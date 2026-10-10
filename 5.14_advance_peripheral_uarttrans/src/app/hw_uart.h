#ifndef HW_UART_H
#define HW_UART_H

#include <stdint.h>

void HWUART_Init();
void HWUART_Printf(const char* format, ...);

// Raw, length-delimited output (no printf formatting).  Used by the NFC
// task's deferred log drain: it writes a bounded number of bytes, so a
// flush can never outlast the reader's 9.7 ms frame waiting time.
void HWUART_Write(const uint8_t *buf, uint16_t len);

#endif
