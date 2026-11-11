/*
 * fm_override.c — Drop-in replacement برای syscalls.c شما
 *
 * پروژه: STM32H743IITX (Cortex-M7)
 *
 *  اگه در پروژه STM32 فایل syscalls.c دارید (یا حتی ندارید)، این
 *  فایل رو به پروژه اضافه کنید. این فایل _sbrk رو safe implement
 *  می‌کنه تا heap overflow نکنه و printf رو خراب نکنه.
 *
 *  همچنین یه my_printf_hal معرفی می‌کنه که از HAL_UART استفاده
 *  می‌کنه و 100% امنه (هیچ malloc نمی‌کنه).
 *
 *  طرز استفاده:
 *    1) این فایل رو به پروژه اضافه کنید
 *    2) در پایین main.c بنویسید: #define printf my_printf_hal
 *       (یا فقط برای عیب‌یابی، نه همیشه)
 *    3) اگه syscalls.c دارید، اون رو از build exclude کنید
 */

/* FM-NOT-FIRMWARE: optional diagnostic, off by default.
 * STM32CubeIDE compiles every .c under FileManager_lib, and it replaces _sbrk and the newlib stubs of syscalls.c / sysmem.c.
 * It compiles to nothing unless the project defines FM_DIAG_SYSCALLS_H7.
 * The rule is checked by tools/check_cubeide.sh. */
#ifdef FM_DIAG_SYSCALLS_H7

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <errno.h>
#include <reent.h>
#include <sys/stat.h>
#include <sys/times.h>
#include <sys/unistd.h>

/* هدر HAL مخصوص STM32H743 */
#include "stm32h7xx_hal.h"

/* این UART هندل رو از پروژه خودتون بیارید. */
/* اگه USART3 دارید (متداول‌ترین برای printf در H7)، بذارید huart3. */
extern UART_HandleTypeDef huart3;
/* extern UART_HandleTypeDef huart1; */   /* ← در صورت نیاز عوض کنید */

/* ------------------------------------------------------------------ */
/*  _sbrk — safe heap allocator                                       */
/* ------------------------------------------------------------------ */
extern char _end;           /* end of BSS (linker symbol)  */
extern char _estack;        /* top of stack (linker symbol) */

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
        if (HAL_UART_Transmit(&huart3, (const uint8_t *)buf, cnt, 100)
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
    (void)buf;
    (void)cnt;
    if (fd == 0) return 0;  /* stdin: not implemented */
    return -1;
}

/* ------------------------------------------------------------------ */
/*  my_printf_hal — printf ایمن با بافر stack (هیچ malloc نمی‌کنه)  */
/* ------------------------------------------------------------------ */
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

    if (HAL_UART_Transmit(&huart3, (const uint8_t *)buf,
                          (uint16_t)n, 100) != HAL_OK)
    {
        return -1;
    }
    return n;
}

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

#else  /* FM_DIAG_SYSCALLS_H7 */
typedef int fm_not_firmware_t;
#endif /* FM_DIAG_SYSCALLS_H7 */
