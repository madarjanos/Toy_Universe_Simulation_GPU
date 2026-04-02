/*
 * pngsave.h
 *
 * PNG file saving via the GDI+ flat C API (Windows, GCC).
 *
 * Created by Claude AI (claude.ai) based on an original C# implementation.
 *
 * GDI+ is part of Windows (gdiplus.dll) and ships with MinGW-w64.
 * Link with: -lgdiplus
 *
 * The flat C API (Gdip* functions) is used instead of the C++ wrapper
 * so this compiles cleanly with gcc -std=c11.
 *
 * Usage:
 *   Call pngsave_init() once at startup.
 *   Call pngsave_hbitmap(path, hbm, w, h) to save a GDI HBITMAP as PNG.
 *   Call pngsave_shutdown() once at exit.
 */

#ifndef PNGSAVE_H
#define PNGSAVE_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>

/* Initialise GDI+.  Must be called once before any pngsave_* call. */
void pngsave_init(void);

/* Shut down GDI+.  Call once at program exit. */
void pngsave_shutdown(void);

/*
 * Save a GDI HBITMAP as a PNG file.
 *
 * Parameters:
 *   path  -- output file path (ANSI)
 *   hbm   -- source HBITMAP (32 bpp DIB section, as created by render.c)
 *   w, h  -- pixel dimensions of the bitmap
 *
 * Returns 1 on success, 0 on failure.
 */
int pngsave_hbitmap(const char *path, HBITMAP hbm, int w, int h);

#endif /* PNGSAVE_H */
