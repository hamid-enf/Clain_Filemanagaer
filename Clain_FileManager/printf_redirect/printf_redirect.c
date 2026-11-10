

#include "printf_redirect.h"  





int __io_putchar(int ch){
	uint8_t c[1];
	c[0]  = ch & 0x00ff;
	HAL_UART_Transmit(&huart1, &c[0], 1, 100);
	return ch;
}


int _write(int file,char *ptr,int len){
	int DataIdex;
	for (DataIdex = 0; DataIdex < len; DataIdex++) {
		__io_putchar(*ptr++);
	}

	return len;
}

int _read(int file, char *ptr, int len)
{
    int i = 0;
    for(i = 0; i < len; i++)
    {
        // دریافت یک بایت از UART1 (می‌تونی Timeout رو هم تغییر بدی)
        if (HAL_UART_Receive(&huart1, (uint8_t *) &ptr[i], 1, HAL_MAX_DELAY) != HAL_OK)
        {
            return -1; // خطا در دریافت
        }

        // اگر Enter زده شد (خط جدید)، خواندن تمام بشه
        if (ptr[i] == '\r') {
            ptr[i] = '\n'; // تطبیق با Line Feed
            i++;
            break;
        }
    }

    return i;
}
