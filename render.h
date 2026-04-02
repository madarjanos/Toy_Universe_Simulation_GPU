/*
 * render.h
 *
 * Off-screen bitmap rendering using Win32 GDI.
 * Corresponds to DrawProcedure / Draw2D / Draw3D in the original C# Form1.cs.
 *
 * Created by Claude AI (claude.ai) based on an original C# implementation.
 *
 * PNG saving is handled by pngsave.c (GDI+).
 * After each render_frame() call, rs->png_last_info contains either:
 *   - "HH:MM:SS path\frameNNNN.png"  (if PNG was saved this frame)
 *   - "PNG: OFF"                      (if PNG saving is disabled)
 * main.c appends this string to the console status line.
 */

#ifndef RENDER_H
#define RENDER_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* -----------------------------------------------------------------------
 * Rendering state — one instance, owned by main()
 * --------------------------------------------------------------------- */
typedef struct render_state
{
    /* User-adjustable view settings (written from the window keyboard handler) */
    volatile double zoom;
    volatile double shift_x;
    volatile double shift_y;
    volatile double shift_z;
    volatile int    use_3d;       /* 1 = perspective 3D, 0 = top-down 2D   */
    volatile int    save_png;     /* 1 = save every frame as PNG            */

    /* 3D rotation angles (radians) */
    volatile double rot_z;
    volatile double rot_x;
    volatile double rot_y;
    volatile int    rot_axis;     /* selected axis: 0=Z 1=X 2=Y            */

    /* PNG output */
    char   png_folder[MAX_PATH];
    int    png_count;

    /* PNG status string — updated each render_frame(), read by main.c
     * to append to the console status line. */
    char   png_last_info[MAX_PATH + 32];

    /* Fade-in status message */
    char   fade_text[128];
    int    fade_counter;

    /* Simulation metadata — updated by main() before each render_frame() */
    int    iswrapped;
    double sim_scale;
    int    sim_timestep;
    int    N;

    /* Working copy of particle positions (allocated in render_init) */
    double *xcopy;
    double *ycopy;
    double *zcopy;

} render_state_t;

/* -----------------------------------------------------------------------
 * Public functions
 * --------------------------------------------------------------------- */

void render_init(render_state_t *rs, int N, int iswrapped,
                 const char *png_folder, int use_3d_default);

void render_free(render_state_t *rs);

void render_frame(render_state_t *rs, HWND hwnd,
                  const float *X, const float *Y, const float *Z,
                  int save_png, double elapsed_ms);

void render_set_fade(render_state_t *rs, const char *text);

#endif /* RENDER_H */
