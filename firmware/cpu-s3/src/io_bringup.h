/*
 * HALTZ CPU: I/O link part of the bring-up screen (docs/io-protocol-v1.md sections 9 and 11,
 * architecture-v1 bring-up step 6). Used by gpu_bringup.c; stands in for the kernel.
 */
#ifndef HALTZ_CPU_IO_BRINGUP_H
#define HALTZ_CPU_IO_BRINGUP_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Two text rows from row (link, input). Loads the pointer sprite. */
void io_bringup_init(int row);
/* After a GPU_INIT: the pointer pattern and sprite again. */
void io_bringup_redraw(void);
/* The two status rows (no PRESENT). */
void io_bringup_draw(void);
/* Handle I/O events: typed keys go to the console, the stick moves the pointer, LED and
 * sounds follow the link and the keyboard. With draw false (the screen is lent to a test)
 * events are still taken but nothing is drawn. Returns true if the picture changed. */
bool io_bringup_poll(bool draw);
/* A key from the USB console: true if it was an I/O key (i p b l c e k). */
bool io_bringup_key(int c);
/* Help text for the keys, and the link statistics. */
void io_bringup_help(void);
void io_bringup_stats(void);

#ifdef __cplusplus
}
#endif

#endif /* HALTZ_CPU_IO_BRINGUP_H */
