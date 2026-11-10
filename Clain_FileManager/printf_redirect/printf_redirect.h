
#ifndef __PRINTF_REDIRECT_H__
#define __PRINTF_REDIRECT_H__

#include "main.h"
#include "usart.h"
#include "stdio.h"
extern UART_HandleTypeDef huart1;
extern int __io_putchar(int ch);
extern int _write(int file, char *ptr, int len);
extern int _read(int file, char *ptr, int len);

#endif
