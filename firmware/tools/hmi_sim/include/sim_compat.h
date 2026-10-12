/* HMI simulator: forced into every file. POSIX and newlib functions the firmware uses that the
 * Microsoft C runtime does not have. */
#pragma once
#include <stddef.h>
#include <string.h>
#include <time.h>
size_t strlcpy(char *dst, const char *src, size_t size);
size_t strlcat(char *dst, const char *src, size_t size);
struct tm *localtime_r(const time_t *t, struct tm *out);
struct tm *gmtime_r(const time_t *t, struct tm *out);
/* hmi_theme.c names the embedded fonts with GCC asm labels; sim_fonts.c defines the plain names. */
#define asm(x)
/* GCC attributes (packed PDO layouts in ecat_drive.h) do not matter to the pages. */
#define __attribute__(x)
