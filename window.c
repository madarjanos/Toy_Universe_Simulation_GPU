/*
 * window.c
 *
 * Win32 window for the N-body simulation visualiser.
 *
 * Created by Claude AI (claude.ai) based on an original C# implementation.
 *
 * B key: sets ws->do_backup = 1.  The main loop detects this, performs
 * the backup (nbody_save), prints the result on a fresh console line,
 * then clears the flag.  The backup itself is NOT done here in the window
 * thread to avoid blocking the message pump.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdio.h>

#include "window.h"
#include "render.h"

#define WM_NEW_BITMAP  (WM_USER + 1)
#define BMP_W 640
#define BMP_H 640

#define ROT_STEP  (5.0 * 3.14159265358979323846 / 180.0)

static const char *CLASS_NAME = "NBodySimWindow";

static window_state_t *s_ws            = NULL;
static HBITMAP         s_currentBitmap = NULL;

/* -----------------------------------------------------------------------
 * Keyboard handler
 * --------------------------------------------------------------------- */
static void handle_key(window_state_t *ws, WPARAM key)
{
    render_state_t *rs = ws->rs;

    switch (key)
    {
    case VK_SPACE:
        ws->paused = !ws->paused;
        render_set_fade(rs, ws->paused ? "PAUSED" : "RESUMED");
        break;

    /* B: request backup — main loop will perform the actual save */
    case 'B':
        ws->do_backup = 1;
        render_set_fade(rs, "Saving backup...");
        break;

    case 'P':
        rs->save_png = !rs->save_png;
        render_set_fade(rs, rs->save_png ? "PNG: ON" : "PNG: OFF");
        break;

    case '2':
        rs->use_3d = 0;
        render_set_fade(rs, "2D mode");
        break;
    case '3':
        rs->use_3d = 1;
        render_set_fade(rs, "3D mode");
        break;

    case VK_OEM_PLUS: case '=': case VK_ADD:
        rs->zoom *= 1.5;
        render_set_fade(rs, "Zoom in");
        break;
    case VK_OEM_MINUS: case VK_SUBTRACT:
        rs->zoom /= 1.5;
        render_set_fade(rs, "Zoom out");
        break;

    case 'D':
        rs->shift_x += 0.05;
        if (rs->iswrapped && rs->shift_x >= 1.0) rs->shift_x -= 1.0;
        render_set_fade(rs, "Shift X+");
        break;
    case 'A':
        rs->shift_x -= 0.05;
        if (rs->iswrapped && rs->shift_x < 0.0) rs->shift_x += 1.0;
        render_set_fade(rs, "Shift X-");
        break;
    case 'W':
        rs->shift_y += 0.05;
        if (rs->iswrapped && rs->shift_y >= 1.0) rs->shift_y -= 1.0;
        render_set_fade(rs, "Shift Y+");
        break;
    case 'S':
        rs->shift_y -= 0.05;
        if (rs->iswrapped && rs->shift_y < 0.0) rs->shift_y += 1.0;
        render_set_fade(rs, "Shift Y-");
        break;
    case 'E':
        rs->shift_z += 0.05;
        if (rs->iswrapped && rs->shift_z >= 1.0) rs->shift_z -= 1.0;
        render_set_fade(rs, "Shift Z+");
        break;
    case 'Q':
        rs->shift_z -= 0.05;
        if (rs->iswrapped && rs->shift_z < 0.0) rs->shift_z += 1.0;
        render_set_fade(rs, "Shift Z-");
        break;

    case 'R':
        rs->shift_x = rs->shift_y = rs->shift_z = 0.0;
        rs->rot_z   = rs->rot_x   = rs->rot_y   = 0.0;
        rs->zoom    = 0.8;
        render_set_fade(rs, "Reset");
        break;

    case 'X':
        rs->rot_axis = 0;
        render_set_fade(rs, "Rotate: Z axis selected");
        break;
    case 'Y':
        rs->rot_axis = 1;
        render_set_fade(rs, "Rotate: X axis selected");
        break;
    case 'Z':
        rs->rot_axis = 2;
        render_set_fade(rs, "Rotate: Y axis selected");
        break;

    case VK_LEFT:
        if      (rs->rot_axis == 0) rs->rot_z -= ROT_STEP;
        else if (rs->rot_axis == 1) rs->rot_x -= ROT_STEP;
        else                        rs->rot_y -= ROT_STEP;
        break;
    case VK_RIGHT:
        if      (rs->rot_axis == 0) rs->rot_z += ROT_STEP;
        else if (rs->rot_axis == 1) rs->rot_x += ROT_STEP;
        else                        rs->rot_y += ROT_STEP;
        break;

    default:
        break;
    }
}

/* -----------------------------------------------------------------------
 * Window procedure
 * --------------------------------------------------------------------- */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg,
                                 WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_NEW_BITMAP:
    {
        HBITMAP hNew = (HBITMAP)lp;
        if (s_currentBitmap) DeleteObject(s_currentBitmap);
        s_currentBitmap = hNew;
        InvalidateRect(hwnd, NULL, FALSE);
        UpdateWindow(hwnd);
        break;
    }

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (s_currentBitmap)
        {
            HDC    mem = CreateCompatibleDC(hdc);
            HBITMAP ol = (HBITMAP)SelectObject(mem, s_currentBitmap);
            BitBlt(hdc, 0, 0, BMP_W, BMP_H, mem, 0, 0, SRCCOPY);
            SelectObject(mem, ol);
            DeleteDC(mem);
        }
        else
        {
            RECT rc; GetClientRect(hwnd, &rc);
            FillRect(hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        }
        EndPaint(hwnd, &ps);
        break;
    }

    case WM_KEYDOWN:
        if (s_ws) handle_key(s_ws, wp);
        break;

    case WM_DESTROY:
        if (s_ws) s_ws->quit = 1;
        PostQuitMessage(0);
        break;

    case WM_GETMINMAXINFO:
    {
        MINMAXINFO *mmi = (MINMAXINFO *)lp;
        mmi->ptMinTrackSize.x = mmi->ptMaxTrackSize.x = BMP_W + 16;
        mmi->ptMinTrackSize.y = mmi->ptMaxTrackSize.y = BMP_H + 39;
        break;
    }

    default:
        return DefWindowProcA(hwnd, msg, wp, lp);
    }
    return 0;
}

/* -----------------------------------------------------------------------
 * window_create
 * --------------------------------------------------------------------- */
HWND window_create(window_state_t *ws, render_state_t *rs, HINSTANCE hInst)
{
    s_ws          = ws;
    ws->rs        = rs;
    ws->quit      = 0;
    ws->paused    = 0;
    ws->do_backup = 0;

    WNDCLASSEXA wc = {0};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = CLASS_NAME;
    wc.hIcon         = LoadIcon(NULL, IDI_APPLICATION);

    if (!RegisterClassExA(&wc))
    {
        fprintf(stderr, "RegisterClassEx failed: %lu\n", GetLastError());
        return NULL;
    }

    RECT  rc    = {0, 0, BMP_W, BMP_H};
    DWORD style = WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX;
    AdjustWindowRect(&rc, style, FALSE);

    HWND hwnd = CreateWindowExA(
        0, CLASS_NAME,
        "N-Body Simulation  "
        "[SPACE=pause  B=backup  P=PNG  2/3=mode  +/-=zoom  "
        "WASD=shiftXY  QE=shiftZ  R=reset  "
        "X/Y/Z=sel.axis  LEFT/RIGHT=rotate]",
        style, CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        NULL, NULL, hInst, NULL);

    if (!hwnd)
    {
        fprintf(stderr, "CreateWindowEx failed: %lu\n", GetLastError());
        return NULL;
    }

    ws->hwnd = hwnd;
    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);
    return hwnd;
}

/* -----------------------------------------------------------------------
 * Message pumps
 * --------------------------------------------------------------------- */
int window_pump(window_state_t *ws)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE))
    {
        if (msg.message == WM_QUIT) { ws->quit = 1; return 0; }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return !ws->quit;
}

void window_run(window_state_t *ws)
{
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    ws->quit = 1;
}
