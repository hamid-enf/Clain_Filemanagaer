/*
 * fm_override.c — Drop-in replacement برای syscalls.c شما
 *
 * اگه در پروژه STM32 فایل syscalls.c دارید (یا حتی ندارید)، این
 * فایل رو به پروژه اضافه کنید. این فایل _sbrk رو safe implement
 * می‌کنه تا heap overflow نکنه و printf رو خراب نکنه.
 *
 * همچنین یه my_printf معرفی می‌کنه که از HAL_UART استفاده می‌کنه
 * و 100% امنه (هیچ malloc نمی‌کنه).
 *
 * طرز استفاده:
 *   1) این فایل رو به پروژه اضافه کنید
 *   2) اگه syscalls.c دارید، اون رو از build exclude کنید
 *   3) در main.c بنویسید: #define printf my_printf_hal
 *   4) یا اگه نمی‌خواهید printf رو عوض کنید، فقط _sbrk safe شما کافیه
 */

/* FM-NOT-FIRMWARE: optional diagnostic, off by default.
 * STM32CubeIDE compiles every .c under FileManager_lib, and it replaces _sbrk and the newlib stubs of syscalls.c / sysmem.c (STM32F4 variant).
 * It compiles to nothing unless the project defines FM_DIAG_SBRK_F4.
 * The rule is checked by tools/check_cubeide.sh. */
#ifdef FM_DIAG_SBRK_F4

#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <reent.h>
#include <sys/stat.h>
#include <sys/times.h>
#include <sys/unistd.h>

/* اگه HAL دارید، include کنید */
#include "stm32f4xx_hal.h"   /* ← اگه STM32F4 هستید */
/* #include "stm32f1xx_hal.h" */  /* ← اگه F1 هستید */
/* #include "stm32h7xx_hal.h" */  /* ← اگه H7 هستید */

/* اگه huart1 جای دیگه‌ای هست، عوض کنید */
extern UART_HandleTypeDef huart1;

/* ------------------------------------------------------------------ */
/*  _sbrk — safe heap allocator                                       */
/*                                                                     */
/*  heap region: [heap_start .. heap_end)                              */
/*  we cap heap_end so it never grows into the stack                   */
/* ------------------------------------------------------------------ */
extern char _end;           /* end of BSS */
extern char _estack;        /* top of stack (linker symbol) */
extern char _Min_Heap_Size; /* optional, may be unused */

static char *s_heap_end = 0;

caddr_t _sbrk(int incr)
{
    char *prev_heap_end;
    char *current_sp;

    if (s_heap_end == 0)
    {
        s_heap_end = &_end;
    }

    prev_heap_end = s_heap_end;

    /* Leave 1 KB of safety margin to stack pointer so a stack
     * overflow can't silently corrupt heap metadata. */
    current_sp = (char *)__get_MSP();
    if (s_heap_end + incr > current_sp - 0x400)
    {
        errno = ENOMEM;
        return (caddr_t)-1;
    }

    s_heap_end += incr;
    return (caddr_t)prev_heap_end;
}

/* ------------------------------------------------------------------ */
/*  _write_r — used by printf to write to stdout (fd=1)               */
/* ------------------------------------------------------------------ */
int _write_r(struct _reent *ptr, int fd, const void *buf, size_t cnt)
{
    if (fd == 1 || fd == 2)  /* stdout or stderr */
    {
        /* HAL_UART_Transmit blocking با timeout 100ms */
        if (HAL_UART_Transmit(&huart1, (const uint8_t *)buf, cnt, 100)
                != HAL_OK)
        {
            ptr->_errno = EIO;
            return -1;
        }
        return (int)cnt;
    }
    ptr->_errno = EBADF;
    return -1;
}

/* ------------------------------------------------------------------ */
/*  _read_r — required by newlib (not used for our case)              */
/* ------------------------------------------------------------------ */
int _read_r(struct _reent *ptr, int fd, void *buf, size_t cnt)
{
    (void)ptr;
    if (fd == 0)
    {
        /* اگه stdin می‌خواهید، اینجا UART receive بذارید */
        return 0;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/*  my_printf_hal — یه printf ایمن که از heap استفاده نمی‌کنه        */
/*                                                                     */
/*  اگه می‌خواهید 100% مطمئن باشید: در main.c بنویسید:               */
/*    #define printf my_printf_hal                                    */
/* ------------------------------------------------------------------ */
#include <stdarg.h>

int my_printf_hal(const char *fmt, ...)
{
    char buf[256];
    va_list args;
    int n;

    va_start(args, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (n < 0) return n;
    if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;

    if (HAL_UART_Transmit(&huart1, (const uint8_t *)buf,
                          (uint16_t)n, 100) != HAL_OK)
    {
        return -1;
    }
    return n;
}

/* اگه نمی‌خواهید printf رو override کنید، کافیه فقط _sbrk و _write_r
 * بالا رو داشته باشید. ولی اگه heap کماکان خراب بود، از my_printf_hal
 * به جای printf استفاده کنید. */

/* ------------------------------------------------------------------ */
/*  سایر توابع مورد نیاز newlib (stubs)                              */
/* ------------------------------------------------------------------ */
int _close_r(struct _reent *ptr, int fd)
{
    (void)ptr;
    (void)fd;
    return 0;
}

int _fstat_r(struct _reent *ptr, int fd, struct stat *st)
{
    (void)ptr;
    (void)fd;
    st->st_mode = S_IFCHR;
    return 0;
}

int _isatty_r(struct _reent *ptr, int fd)
{
    (void)ptr;
    (void)fd;
    return 1;
}

off_t _lseek_r(struct _reent *ptr, int fd, off_t offset, int whence)
{
    (void)ptr;
    (void)fd;
    (void)offset;
    (void)whence;
    return 0;
}

int _getpid(void)
{
    return 1;
}

int _kill(int pid, int sig)
{
    (void)pid;
    (void)sig;
    return -1;
}

clock_t _times(struct tms *buf)
{
    (void)buf;
    return (clock_t)-1;
}

int _wait(int *status)
{
    (void)status;
    return -1;
}

#else  /* FM_DIAG_SBRK_F4 */
typedef int fm_not_firmware_t;
#endif /* FM_DIAG_SBRK_F4 */
