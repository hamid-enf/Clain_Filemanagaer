# tools/rtosstub — host stand-ins for the RTOS kernels

The FreeRTOS and CMSIS-RTOS2 sync ports (`ports/freertos`,
`ports/cmsis_rtos2`) can only be compiled against their kernel headers,
which are not part of this repository.  These directories provide the
handful of declarations the ports use, **with the kernel's own types and
signatures**, and implement them on top of pthreads (`rtos_host.c`).

That makes the two port headers part of the host test run:

    make clean && make fm_test FM_USE_THREADS=1 FM_SYNC_IMPL=freertos
    make clean && make fm_test FM_USE_THREADS=1 FM_SYNC_IMPL=cmsis_rtos2
    ./fm_test

The whole suite, including the multi-thread stress tests, then runs
through the unmodified `ports/<rtos>/fm_sync_impl.h`.  Nothing in here
is meant for a target build — on the MCU the real kernel is used.
