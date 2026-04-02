/*
 * render.c
 *
 * Off-screen 640x640 bitmap rendering with Win32 GDI.
 *
 * Created by Claude AI (claude.ai) based on an original C# implementation
 * (Form1.cs: DrawProcedure, Draw2D, Draw3D, Projector3D).
 *
 * PNG saving is delegated to pngsave.c (GDI+ flat C API).
 * After render_frame() returns, rs->png_last_info is set to either:
 *   "HH:MM:SS path\frameNNNN.png"   -- if a PNG was saved this frame
 *   "PNG: OFF"                       -- if PNG saving is disabled
 * main.c appends this to the console status line.
 *
 * The self-contained store-only PNG encoder has been removed.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "render.h"
#include "pngsave.h"

#define WM_NEW_BITMAP  (WM_USER + 1)
#define BMP_W 640
#define BMP_H 640

/* -----------------------------------------------------------------------
 * 3D perspective projection
 * Eye: (0.5, 0.5, -1.5)  View direction: +Z  Focal length: 1.2
 * --------------------------------------------------------------------- */
#define EYE_X  0.5
#define EYE_Y  0.5
#define EYE_Z -1.5
#define FOCAL  1.2
#define ROT_Z_START 20*3.14159265/180
#define ROT_X_START 30*3.14159265/180
#define ROT_Y_START 60*3.14159265/180

static int project3d(double wx, double wy, double wz,
                     int w, int h, double *sx, double *sy)
{
    double dz = wz - EYE_Z;
    if (dz < 1e-6) { *sx = *sy = -1.0; return 0; }
    double t = FOCAL / dz;
    *sx = ((wx - EYE_X) * t + 0.5) * w;
    *sy = ((wy - EYE_Y) * t + 0.5) * h;
    return 1;
}

/* -----------------------------------------------------------------------
 * Rotation in a 2D plane around the midpoint 0.5
 * --------------------------------------------------------------------- */
static void rotate2d(double *a, double *b, double angle)
{
    double ca = cos(angle), sa = sin(angle);
    double da = *a - 0.5, db = *b - 0.5;
    *a = ca * da - sa * db + 0.5;
    *b = sa * da + ca * db + 0.5;
}

/* -----------------------------------------------------------------------
 * render_init / render_free / render_set_fade
 * --------------------------------------------------------------------- */
void render_init(render_state_t *rs, int N, int iswrapped,
                 const char *png_folder, int use_3d_default)
{
    memset(rs, 0, sizeof(*rs));
    rs->zoom       = 0.8;
    rs->use_3d     = use_3d_default;
    rs->iswrapped  = iswrapped;
    rs->N          = N;
    rs->rot_z      = ROT_Z_START;
    rs->rot_x      = ROT_X_START;
    rs->rot_y      = ROT_Y_START;
    rs->rot_axis   = 0;
    strncpy(rs->png_folder, png_folder, MAX_PATH - 1);
    strncpy(rs->png_last_info, "PNG: OFF", sizeof(rs->png_last_info) - 1);

    rs->xcopy = (double *)malloc(N * sizeof(double));
    rs->ycopy = (double *)malloc(N * sizeof(double));
    rs->zcopy = (double *)malloc(N * sizeof(double));
    if (!rs->xcopy || !rs->ycopy || !rs->zcopy)
    { fprintf(stderr, "render_init: out of memory\n"); exit(1); }
}

void render_free(render_state_t *rs)
{
    free(rs->xcopy); free(rs->ycopy); free(rs->zcopy);
    rs->xcopy = rs->ycopy = rs->zcopy = NULL;
}

void render_set_fade(render_state_t *rs, const char *text)
{
    strncpy(rs->fade_text, text, sizeof(rs->fade_text) - 1);
    rs->fade_counter = 24;
}

/* -----------------------------------------------------------------------
 * DIB creation: 32 bpp top-down.
 * *pixels_out is valid as long as hbm is alive.
 * --------------------------------------------------------------------- */
static HBITMAP create_dib(int w, int h, uint32_t **pixels_out)
{
    BITMAPINFOHEADER bih = {0};
    bih.biSize        = sizeof(bih);
    bih.biWidth       = w;
    bih.biHeight      = -h;
    bih.biPlanes      = 1;
    bih.biBitCount    = 32;
    bih.biCompression = BI_RGB;

    void *bits = NULL;
    HBITMAP hbm = CreateDIBSection(NULL, (BITMAPINFO *)&bih,
                                   DIB_RGB_COLORS, &bits, NULL, 0);
    *pixels_out = (uint32_t *)bits;
    return hbm;
}

/* -----------------------------------------------------------------------
 * render_frame
 * --------------------------------------------------------------------- */
void render_frame(render_state_t *rs, HWND hwnd,
                  const float *X, const float *Y, const float *Z,
                  int save_png_flag, double elapsed_ms)
{
    int    w          = BMP_W, h = BMP_H;
    int    N          = rs->N;
    int    iswrapped  = rs->iswrapped;
    double zoom       = rs->zoom;
    double shift_x    = rs->shift_x;
    double shift_y    = rs->shift_y;
    double shift_z    = rs->shift_z;
    double sim_scale  = rs->sim_scale;
    double rot_z      = rs->rot_z;
    double rot_x      = rs->rot_x;
    double rot_y      = rs->rot_y;

    /* --- 1. Copy and shift particle positions --- */
    if (iswrapped)
    {
        for (int i = 0; i < N; i++)
        {
            double v;
            v = (double)X[i] + shift_x; rs->xcopy[i] = v - floor(v);
            v = (double)Y[i] + shift_y; rs->ycopy[i] = v - floor(v);
            v = (double)Z[i] + shift_z; rs->zcopy[i] = v - floor(v);
        }
    }
    else
    {
        for (int i = 0; i < N; i++)
        {
            rs->xcopy[i] = (double)X[i] + shift_x;
            rs->ycopy[i] = (double)Y[i] + shift_y;
            rs->zcopy[i] = (double)Z[i] + shift_z;
        }
    }

    /* --- 2. Create DIB and HDC --- */
    uint32_t *pix = NULL;
    HBITMAP   hbm = create_dib(w, h, &pix);
    if (!hbm) return;

    HDC     hdc  = CreateCompatibleDC(NULL);
    HBITMAP hOld = (HBITMAP)SelectObject(hdc, hbm);

    RECT full = {0, 0, w, h};
    FillRect(hdc, &full, (HBRUSH)GetStockObject(BLACK_BRUSH));

    double scale_draw = zoom * sim_scale;
#define RESCALE(x)  (((x) - 0.5) * scale_draw + 0.5)
#define TO_PX(x)    ((int)(RESCALE(x) * w))
#define TO_PY(y)    ((int)(RESCALE(y) * h))

    /* ================================================================
     * 3a. 2D mode
     * ============================================================== */
    if (!rs->use_3d)
    {
        if (iswrapped)
        {
            HPEN hpen = CreatePen(PS_SOLID, 1, RGB(255, 0, 0));
            HPEN hold = (HPEN)SelectObject(hdc, hpen);
            double cx[4]={0,1,1,0}, cy[4]={0,0,1,1};
            for (int i = 0; i < 4; i++)
            {
                MoveToEx(hdc, TO_PX(cx[i]),       TO_PY(cy[i]),       NULL);
                LineTo  (hdc, TO_PX(cx[(i+1)%4]), TO_PY(cy[(i+1)%4]));
            }
            SelectObject(hdc, hold);
            DeleteObject(hpen);
        }

        HBRUSH hbrush = CreateSolidBrush(RGB(255, 255, 255));
        HBRUSH hboldb = (HBRUSH)SelectObject(hdc, hbrush);
        HPEN   hnopen = (HPEN)SelectObject(hdc, GetStockObject(NULL_PEN));
        for (int i = 0; i < N; i++)
        {
            int px = TO_PX(rs->xcopy[i]);
            int py = TO_PY(rs->ycopy[i]);
            Rectangle(hdc, px, py, px + 2, py + 2);
        }
        SelectObject(hdc, hboldb);
        SelectObject(hdc, hnopen);
        DeleteObject(hbrush);
    }
    /* ================================================================
     * 3b. 3D mode: RESCALE → rotate(Z,X,Y) → project
     * ============================================================== */
    else
    {
        if (iswrapped)
        {
            double corners[8][3] = {
                {0,0,0},{1,0,0},{1,1,0},{0,1,0},
                {0,0,1},{1,0,1},{1,1,1},{0,1,1}
            };
            int edges[12][2] = {
                {0,1},{1,2},{2,3},{3,0},
                {4,5},{5,6},{6,7},{7,4},
                {0,4},{1,5},{2,6},{3,7}
            };
            double cpx[8], cpy[8]; int cvis[8];
            for (int i = 0; i < 8; i++)
            {
                double cx = RESCALE(corners[i][0]);
                double cy = RESCALE(corners[i][1]);
                double cz = RESCALE(corners[i][2]);
                rotate2d(&cx, &cy, rot_z);
                rotate2d(&cy, &cz, rot_x);
                rotate2d(&cz, &cx, rot_y);
                cvis[i] = project3d(cx, cy, cz, w, h, &cpx[i], &cpy[i]);
            }
            HPEN hpen = CreatePen(PS_SOLID, 1, RGB(255, 0, 0));
            HPEN hold = (HPEN)SelectObject(hdc, hpen);
            for (int ei = 0; ei < 12; ei++)
            {
                int a = edges[ei][0], b = edges[ei][1];
                if (cvis[a] && cvis[b])
                {
                    MoveToEx(hdc, (int)cpx[a], (int)cpy[a], NULL);
                    LineTo  (hdc, (int)cpx[b], (int)cpy[b]);
                }
            }
            SelectObject(hdc, hold);
            DeleteObject(hpen);
        }

        HBRUSH hbrush = CreateSolidBrush(RGB(255, 255, 255));
        HBRUSH hboldb = (HBRUSH)SelectObject(hdc, hbrush);
        HPEN   hnopen = (HPEN)SelectObject(hdc, GetStockObject(NULL_PEN));
        for (int i = 0; i < N; i++)
        {
            double rx = RESCALE(rs->xcopy[i]);
            double ry = RESCALE(rs->ycopy[i]);
            double rz = RESCALE(rs->zcopy[i]);
            rotate2d(&rx, &ry, rot_z);
            rotate2d(&ry, &rz, rot_x);
            rotate2d(&rz, &rx, rot_y);
            double sx, sy;
            if (project3d(rx, ry, rz, w, h, &sx, &sy))
            {
                int px = (int)sx, py = (int)sy;
                Rectangle(hdc, px, py, px + 2, py + 2);
            }
        }
        SelectObject(hdc, hboldb);
        SelectObject(hdc, hnopen);
        DeleteObject(hbrush);
    }

#undef RESCALE
#undef TO_PX
#undef TO_PY

    /* --- 4. HUD text --- */
    {
        const char *axis_names[] = {"Z", "X", "Y"};
        char header[320];
        if (rs->use_3d)
            snprintf(header, sizeof(header),
                "Step:%-7d Scale:%.2f N:%d [3D] "
                "RotZ:%.0f RotX:%.0f RotY:%.0f Sel:%s",
                rs->sim_timestep, sim_scale, N,
                rot_z * 180.0 / 3.14159265,
                rot_x * 180.0 / 3.14159265,
                rot_y * 180.0 / 3.14159265,
                axis_names[rs->rot_axis], elapsed_ms);
        else
            snprintf(header, sizeof(header),
                "Step:%-7d Scale:%.2f N:%d [2D]",
                rs->sim_timestep, sim_scale, N, elapsed_ms);

        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(255, 255, 255));

        HFONT hfont = CreateFontA(
            16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, "Courier New");
        HFONT holdfont = (HFONT)SelectObject(hdc, hfont);

        SIZE sz;
        GetTextExtentPoint32A(hdc, header, (int)strlen(header), &sz);
        TextOutA(hdc, (w - sz.cx) / 2, 4, header, (int)strlen(header));

        /* --- 5. Fade message --- */
        if (rs->fade_counter > 0)
        {
            int bright = (rs->fade_counter * 255) / 24;
            SetTextColor(hdc, RGB(bright, bright, bright));
            GetTextExtentPoint32A(hdc, rs->fade_text,
                                  (int)strlen(rs->fade_text), &sz);
            TextOutA(hdc, (w - sz.cx) / 2, 20,
                     rs->fade_text, (int)strlen(rs->fade_text));
            rs->fade_counter--;
        }

        SelectObject(hdc, holdfont);
        DeleteObject(hfont);
    }

    /* --- 6. Release HDC --- */
    SelectObject(hdc, hOld);
    DeleteDC(hdc);

    /* --- 7. PNG save via GDI+ (pngsave.c) ---
     * Update png_last_info for the console status line.
     * If save_png is off: just write "PNG: OFF".
     * If save_png is on:  build path, save, write "HH:MM:SS <path>". */
    if (!save_png_flag)
    {
        strncpy(rs->png_last_info, "PNG: OFF",
                sizeof(rs->png_last_info) - 1);
    }
    else
    {
        char path[MAX_PATH + 32];
        snprintf(path, sizeof(path), "%s\\frame%04d.png",
                 rs->png_folder, rs->png_count);

        if (pngsave_hbitmap(path, hbm, w, h))
        {
            /* Build timestamp HH:MM:SS */
            SYSTEMTIME st;
            GetLocalTime(&st);
            snprintf(rs->png_last_info, sizeof(rs->png_last_info),
                     "%02d:%02d:%02d %s",
                     st.wHour, st.wMinute, st.wSecond, path);
            rs->png_count++;
        }
        else
        {
            snprintf(rs->png_last_info, sizeof(rs->png_last_info),
                     "PNG ERROR: %s", path);
        }
    }

    /* --- 8. Hand bitmap to window via PostMessage --- */
    PostMessage(hwnd, WM_NEW_BITMAP, 0, (LPARAM)hbm);
}
