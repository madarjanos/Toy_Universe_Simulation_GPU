/*
 * window.h
 *
 * Win32 window management for the N-body simulation visualiser.
 *
 * Created by Claude AI (claude.ai) based on an original C# implementation.
 *
 * Keyboard bindings:
 *   SPACE         pause / resume simulation
 *   B             save backup (state + parameters) to outdir folder
 *   P             toggle PNG frame saving
 *   2 / 3         switch to 2D / 3D rendering mode
 *   + or =        zoom in  (x1.5)
 *   -             zoom out (/1.5)
 *   W / S         shift Y axis +/-
 *   A / D         shift X axis +/-
 *   Q / E         shift Z axis +/-
 *   R             reset zoom, shifts and rotation angles
 *   X             select Z-axis rotation  (LEFT/RIGHT then rotate)
 *   Y             select X-axis rotation
 *   Z             select Y-axis rotation
 *   LEFT arrow    rotate selected axis by -5 degrees
 *   RIGHT arrow   rotate selected axis by +5 degrees
 */

#ifndef WINDOW_H
#define WINDOW_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "render.h"

/* -----------------------------------------------------------------------
 * Window state — one instance, owned by main()
 * --------------------------------------------------------------------- */
typedef struct window_state
{
    HWND           hwnd;
    volatile int   quit;        /* set to 1 when the window is closed      */
    volatile int   paused;      /* set to 1 while simulation is paused     */
    volatile int   do_backup;   /* set to 1 when B is pressed; main clears */
    render_state_t *rs;         /* back-pointer for keyboard handlers      */
} window_state_t;

/* -----------------------------------------------------------------------
 * Public functions
 * --------------------------------------------------------------------- */
HWND window_create(window_state_t *ws, render_state_t *rs, HINSTANCE hInst);
int  window_pump(window_state_t *ws);
void window_run(window_state_t *ws);

#endif /* WINDOW_H */
