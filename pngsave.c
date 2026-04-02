/*
 * pngsave.c
 *
 * PNG file saving via the GDI+ flat C API (Windows, GCC / MinGW-w64).
 *
 * Created by Claude AI (claude.ai) based on an original C# implementation.
 *
 * Why GDI+ flat C API instead of the C++ wrapper?
 *   The standard <gdiplus.h> header is C++-only (uses classes, namespaces,
 *   operator overloads).  MinGW-w64 ships a flat procedural C API in
 *   <gdiplus.h> / gdiplus.dll that mirrors the internal COM-like interface.
 *   We declare only the symbols we need, avoiding the C++ header entirely.
 *   This keeps the whole project in plain C11.
 *
 * Link: -lgdiplus
 *
 * Approach:
 *   1. Create a GDI+ Bitmap from the existing HBITMAP via
 *      GdipCreateBitmapFromHBITMAP.
 *   2. Look up the PNG encoder CLSID once (cached after first call).
 *   3. Save with GdipSaveImageToFile (wide-char path).
 *   4. Delete the GDI+ bitmap wrapper (the original HBITMAP is untouched).
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pngsave.h"

/* -----------------------------------------------------------------------
 * Minimal GDI+ flat C declarations
 * (avoids including the C++-only <gdiplus.h>)
 * --------------------------------------------------------------------- */

/* GDI+ status codes */
typedef int GpStatus;
#define GP_OK  0

/* Opaque GDI+ object handles */
typedef void GpBitmap;
typedef void GpImage;

/* GDI+ startup / shutdown structures */
typedef struct GdiplusStartupInput_
{
    UINT32   GdiplusVersion;
    void    *DebugEventCallback;   /* NULL */
    BOOL     SuppressBackgroundThread;
    BOOL     SuppressExternalCodecs;
} GdiplusStartupInput;

typedef struct GdiplusStartupOutput_
{
    void *NotificationHook;
    void *NotificationUnhook;
} GdiplusStartupOutput;

/* EncoderParameters (we need a minimal version for SaveImageToFile) */
typedef struct EncoderParameters_ { UINT32 Count; } EncoderParameters;

/* GDI+ function pointer types */
typedef GpStatus (WINAPI *PFN_GdiplusStartup)(
    ULONG_PTR *, const GdiplusStartupInput *, GdiplusStartupOutput *);
typedef void     (WINAPI *PFN_GdiplusShutdown)(ULONG_PTR);
typedef GpStatus (WINAPI *PFN_GdipCreateBitmapFromHBITMAP)(
    HBITMAP, HPALETTE, GpBitmap **);
typedef GpStatus (WINAPI *PFN_GdipSaveImageToFile)(
    GpImage *, const WCHAR *, const CLSID *, const EncoderParameters *);
typedef GpStatus (WINAPI *PFN_GdipDisposeImage)(GpImage *);
typedef GpStatus (WINAPI *PFN_GdipGetImageEncodersSize)(UINT *, UINT *);
typedef GpStatus (WINAPI *PFN_GdipGetImageEncoders)(UINT, UINT, void *);

/* -----------------------------------------------------------------------
 * ImageCodecInfo layout (from wingdi / gdiplus headers)
 * We only need the Clsid and MimeType fields.
 * --------------------------------------------------------------------- */
typedef struct ImageCodecInfo_
{
    CLSID  Clsid;
    GUID   FormatID;
    WCHAR *CodecName;
    WCHAR *DllName;
    WCHAR *FormatDescription;
    WCHAR *FilenameExtension;
    WCHAR *MimeType;
    DWORD  Flags;
    DWORD  Version;
    DWORD  SigCount;
    DWORD  SigSize;
    BYTE  *SigPattern;
    BYTE  *SigMask;
} ImageCodecInfo;

/* -----------------------------------------------------------------------
 * Module-level state
 * --------------------------------------------------------------------- */
static HMODULE  s_hgdiplus = NULL;
static ULONG_PTR s_token   = 0;

static PFN_GdiplusStartup              s_Startup             = NULL;
static PFN_GdiplusShutdown             s_Shutdown            = NULL;
static PFN_GdipCreateBitmapFromHBITMAP s_CreateBitmapFromHBM = NULL;
static PFN_GdipSaveImageToFile         s_SaveImageToFile     = NULL;
static PFN_GdipDisposeImage            s_DisposeImage        = NULL;
static PFN_GdipGetImageEncodersSize    s_GetEncodersSize     = NULL;
static PFN_GdipGetImageEncoders        s_GetEncoders         = NULL;

/* Cached PNG encoder CLSID (looked up once) */
static CLSID s_png_clsid;
static int   s_png_clsid_ready = 0;

/* -----------------------------------------------------------------------
 * Helper: load a function from gdiplus.dll; abort on failure
 * --------------------------------------------------------------------- */
static void *load_proc(const char *name)
{
    void *p = (void *)GetProcAddress(s_hgdiplus, name);
    if (!p)
    {
        fprintf(stderr, "pngsave: cannot find %s in gdiplus.dll\n", name);
        exit(1);
    }
    return p;
}

/* -----------------------------------------------------------------------
 * pngsave_init — load gdiplus.dll and start up GDI+
 * --------------------------------------------------------------------- */
void pngsave_init(void)
{
    s_hgdiplus = LoadLibraryA("gdiplus.dll");
    if (!s_hgdiplus)
    {
        fprintf(stderr, "pngsave: cannot load gdiplus.dll\n");
        exit(1);
    }

    s_Startup             = (PFN_GdiplusStartup)             load_proc("GdiplusStartup");
    s_Shutdown            = (PFN_GdiplusShutdown)            load_proc("GdiplusShutdown");
    s_CreateBitmapFromHBM = (PFN_GdipCreateBitmapFromHBITMAP)load_proc("GdipCreateBitmapFromHBITMAP");
    s_SaveImageToFile     = (PFN_GdipSaveImageToFile)         load_proc("GdipSaveImageToFile");
    s_DisposeImage        = (PFN_GdipDisposeImage)            load_proc("GdipDisposeImage");
    s_GetEncodersSize     = (PFN_GdipGetImageEncodersSize)    load_proc("GdipGetImageEncodersSize");
    s_GetEncoders         = (PFN_GdipGetImageEncoders)        load_proc("GdipGetImageEncoders");

    GdiplusStartupInput inp = {1, NULL, FALSE, FALSE};
    GpStatus st = s_Startup(&s_token, &inp, NULL);
    if (st != GP_OK)
    {
        fprintf(stderr, "pngsave: GdiplusStartup failed (%d)\n", st);
        exit(1);
    }
}

/* -----------------------------------------------------------------------
 * pngsave_shutdown
 * --------------------------------------------------------------------- */
void pngsave_shutdown(void)
{
    if (s_Shutdown && s_token)
        s_Shutdown(s_token);
    if (s_hgdiplus)
        FreeLibrary(s_hgdiplus);
    s_token    = 0;
    s_hgdiplus = NULL;
}

/* -----------------------------------------------------------------------
 * find_png_encoder — look up the PNG CLSID from GDI+ (cached)
 * --------------------------------------------------------------------- */
static int find_png_encoder(void)
{
    if (s_png_clsid_ready) return 1;

    UINT num = 0, sz = 0;
    s_GetEncodersSize(&num, &sz);
    if (sz == 0) return 0;

    ImageCodecInfo *codecs = (ImageCodecInfo *)malloc(sz);
    s_GetEncoders(num, sz, codecs);

    int found = 0;
    for (UINT i = 0; i < num; i++)
    {
        /* Match on MIME type L"image/png" */
        if (codecs[i].MimeType &&
            wcscmp(codecs[i].MimeType, L"image/png") == 0)
        {
            s_png_clsid       = codecs[i].Clsid;
            s_png_clsid_ready = 1;
            found = 1;
            break;
        }
    }
    free(codecs);
    return found;
}

/* -----------------------------------------------------------------------
 * pngsave_hbitmap — save an HBITMAP as PNG via GDI+
 * --------------------------------------------------------------------- */
int pngsave_hbitmap(const char *path, HBITMAP hbm, int w, int h)
{
    (void)w; (void)h;  /* GDI+ reads dimensions from the bitmap itself */

    if (!find_png_encoder())
    {
        fprintf(stderr, "pngsave: PNG encoder not found\n");
        return 0;
    }

    /* Convert ANSI path to wide string */
    WCHAR wpath[MAX_PATH + 32];
    MultiByteToWideChar(CP_ACP, 0, path, -1, wpath,
                        sizeof(wpath) / sizeof(WCHAR));

    /* Wrap the HBITMAP in a GDI+ Bitmap object */
    GpBitmap *bmp = NULL;
    GpStatus st = s_CreateBitmapFromHBM(hbm, NULL, &bmp);
    if (st != GP_OK || !bmp)
    {
        fprintf(stderr, "pngsave: GdipCreateBitmapFromHBITMAP failed (%d)\n", st);
        return 0;
    }

    /* Save as PNG */
    st = s_SaveImageToFile((GpImage *)bmp, wpath, &s_png_clsid, NULL);
    s_DisposeImage((GpImage *)bmp);

    if (st != GP_OK)
    {
        fprintf(stderr, "pngsave: GdipSaveImageToFile failed (%d)\n", st);
        return 0;
    }
    return 1;
}
