/*
 * main.c
 *
 * Entry point for the N-body gravitational simulation.
 *
 * Created by Claude AI (claude.ai) based on an original C# implementation.
 *
 * Command-line arguments (all optional unless noted):
 *
 *   Simulation parameters (ignored when --load is used):
 *   --n <int>              Number of particles            (default: 10000)
 *   --steps <int>          Simulation steps per frame     (default: 100)
 *   --wrapped <0|1>        Pac-Man periodic boundary      (default: 1)
 *   --dt <float>           Time step size                 (default: 2e-6)
 *   --g <float>            Gravitational constant         (default: 1.0)
 *   --soft <float>         Softening length               (default: 1e-3)
 *   --tile <int>           GPU work-group / tile size     (default: 256)
 *   --expansion <float>    Expansion factor               (default: 20.0)
 *   --cl <path>            Path to nbody_kernels.cl       (default: nbody_kernels.cl)
 *
 *   Load / save:
 *   --load <path>          Load a backup file and resume from it.
 *                          Simulation parameters above are ignored.
 *                          Render parameters below still apply.
 *
 *   Render parameters (always active):
 *   --outdir <path>        PNG / backup output folder     (default: .)
 *   --2d                   Start in 2D mode
 *   --3d                   Start in 3D mode               (default)
 *
 *   --help                 Print usage and keyboard controls
 *
 * Backup (B key):
 *   Saves the complete simulation state to outdir\nbody_YYYYMMDD_HHMMSS.bak
 *   The file can be loaded later with --load.  The save path and timestamp
 *   are printed on a fresh console line (not overwriting the status line).
 *
 * Compile:
 *   gcc -O3 -march=native -ffast-math -std=c11 -o nbodysim.exe ^
 *       main.c nbody_simulation.c nbody_save.c render.c window.c pngsave.c ^
 *       -I C:\opencl-sdk\include -L C:\opencl-sdk\lib ^
 *       -lOpenCL -lgdi32 -luser32 -lgdiplus
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "nbody_simulation.h"
#include "nbody_save.h"
#include "render.h"
#include "window.h"
#include "pngsave.h"

/* -----------------------------------------------------------------------
 * Console: in-place status line
 * --------------------------------------------------------------------- */
static int s_last_status_len = 0;

static void print_status(const char *fmt, ...)
{
    char buf[640];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    int len = (int)strlen(buf);
    printf("\r");
    for (int i = 0; i < s_last_status_len; i++) putchar(' ');
    printf("\r%s", buf);
    fflush(stdout);
    s_last_status_len = len;
}

/* Print a message on a fresh line, then resume the status line.
 * Use for rare important events (backup saved, PNG save error, etc.). */
static void print_notice(const char *fmt, ...)
{
    char buf[640];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    /* Erase current status line, print notice, reset counter so the
     * next print_status call starts on a clean line. */
    printf("\r");
    for (int i = 0; i < s_last_status_len; i++) putchar(' ');
    printf("\r%s\n", buf);
    fflush(stdout);
    s_last_status_len = 0;
}

/* -----------------------------------------------------------------------
 * Keyboard help
 * --------------------------------------------------------------------- */
static void print_keyboard_help(void)
{
    printf(
        "\n"
        "Window keyboard controls:\n"
        "  SPACE          pause / resume simulation\n"
        "  B              save backup to outdir (resume later with --load)\n"
        "  P              toggle PNG frame saving (to --outdir folder)\n"
        "  2              switch to 2D top-down mode\n"
        "  3              switch to 3D perspective mode\n"
        "  + / =          zoom in  (x1.5)\n"
        "  -              zoom out (/1.5)\n"
        "  W / S          shift view along Y axis +/-\n"
        "  A / D          shift view along X axis +/-\n"
        "  Q / E          shift view along Z axis +/-\n"
        "  R              reset zoom, shift and rotation angles\n"
        "  X              select Z-axis rotation (LEFT/RIGHT to rotate)\n"
        "  Y              select X-axis rotation\n"
        "  Z              select Y-axis rotation\n"
        "  LEFT arrow     rotate selected axis by -5 degrees\n"
        "  RIGHT arrow    rotate selected axis by +5 degrees\n"
        "\n"
    );
}

/* -----------------------------------------------------------------------
 * Configuration
 * --------------------------------------------------------------------- */
typedef struct config
{
    /* Simulation parameters — ignored when load_path is set */
    int                N;
    int                steps_per_render;
    int                wrapped;
    double             dt;
    double             g;
    double             soft;
    int                tile;
    double             expansion;
    unsigned long long randseed;
    char               clpath[MAX_PATH];

    /* Load / save */
    char   load_path[MAX_PATH];   /* empty string = no load */

    /* Render parameters — always active */
    char   outdir[MAX_PATH];
    int    use_3d;
} config_t;

static void config_defaults(config_t *c)
{
    c->N                = 10000;
    c->steps_per_render = 100;
    c->wrapped          = 1;
    c->dt               = 2e-6;
    c->g                = 1.0;
    c->soft             = 1e-3;
    c->tile             = 256;
    c->expansion        = 20.0;
    c->randseed         = 301;
    c->use_3d           = 1;
    c->load_path[0]     = '\0';
    strcpy(c->outdir, ".");
    strcpy(c->clpath, "nbody_kernels.cl");
}

static void print_usage(const char *exe)
{
    printf(
        "Usage: %s [options]\n"
        "\n"
        "Simulation parameters (ignored with --load):\n"
        "  --n <int>           Number of particles          (default: 10000)\n"
        "  --steps <int>       Simulation steps per frame   (default: 100)\n"
        "  --wrapped <0|1>     Pac-Man periodic boundary    (default: 1)\n"
        "  --dt <float>        Time step                    (default: 2e-6)\n"
        "  --g <float>         Gravitational constant       (default: 1.0)\n"
        "  --soft <float>      Softening length             (default: 1e-3)\n"
        "  --tile <int>        GPU work-group size          (default: 256)\n"
        "  --expansion <float> Expansion factor             (default: 20.0)\n"
        "  --randseed <int>    Random seed for particles    (default: 301)\n"
        "  --cl <path>         OpenCL kernel file           (default: nbody_kernels.cl)\n"
        "\n"
        "Load / resume:\n"
        "  --load <path>       Load a .bak backup file and resume simulation.\n"
        "                      Simulation parameters above are taken from the file.\n"
        "\n"
        "Render parameters (always active):\n"
        "  --outdir <dir>      PNG and backup output folder (default: .)\n"
        "  --2d                Start in 2D top-down mode\n"
        "  --3d                Start in 3D perspective mode (default)\n"
        "  --help              Show this message\n",
        exe);
    print_keyboard_help();
}

static int parse_args(int argc, char **argv, config_t *c)
{
    /* Track which simulation params were explicitly set.
     * If --load is used and any are set, we warn but proceed (load wins). */
    int sim_params_set = 0;

    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h"))
        { print_usage(argv[0]); return 0; }

#define NEED() if(i+1>=argc){fprintf(stderr,"Missing arg: %s\n",argv[i]);return 0;}
        else if (!strcmp(argv[i],"--n"))         { NEED(); c->N                =atoi(argv[++i]); sim_params_set=1; }
        else if (!strcmp(argv[i],"--steps"))     { NEED(); c->steps_per_render =atoi(argv[++i]); sim_params_set=1; }
        else if (!strcmp(argv[i],"--wrapped"))   { NEED(); c->wrapped          =atoi(argv[++i]); sim_params_set=1; }
        else if (!strcmp(argv[i],"--dt"))        { NEED(); c->dt               =atof(argv[++i]); sim_params_set=1; }
        else if (!strcmp(argv[i],"--g"))         { NEED(); c->g                =atof(argv[++i]); sim_params_set=1; }
        else if (!strcmp(argv[i],"--soft"))      { NEED(); c->soft             =atof(argv[++i]); sim_params_set=1; }
        else if (!strcmp(argv[i],"--tile"))      { NEED(); c->tile             =atoi(argv[++i]); sim_params_set=1; }
        else if (!strcmp(argv[i],"--expansion")) { NEED(); c->expansion        =atof(argv[++i]); sim_params_set=1; }
        else if (!strcmp(argv[i],"--randseed")) { NEED(); c->randseed =(unsigned long long)strtoull(argv[++i],NULL,10); sim_params_set=1; }
        else if (!strcmp(argv[i],"--cl"))        { NEED(); strncpy(c->clpath, argv[++i],MAX_PATH-1); sim_params_set=1; }
        else if (!strcmp(argv[i],"--load"))      { NEED(); strncpy(c->load_path,argv[++i],MAX_PATH-1); }
        else if (!strcmp(argv[i],"--outdir"))    { NEED(); strncpy(c->outdir,argv[++i],MAX_PATH-1); }
        else if (!strcmp(argv[i],"--2d"))        { c->use_3d = 0; }
        else if (!strcmp(argv[i],"--3d"))        { c->use_3d = 1; }
        else
        { fprintf(stderr,"Unknown: %s\n\n",argv[i]); print_usage(argv[0]); return 0; }
#undef NEED
    }

    /* Warn if simulation params were given together with --load */
    if (c->load_path[0] && sim_params_set)
    {
        fprintf(stderr,
            "Warning: simulation parameters (--n, --dt, etc.) are ignored "
            "when --load is used; parameters are taken from the backup file.\n");
    }

    return 1;
}

/* -----------------------------------------------------------------------
 * Backup handler — called from the main loop when do_backup is set
 * --------------------------------------------------------------------- */
static void do_backup(const char *outdir)
{
    /* Build filename: outdir\nbody_YYYYMMDD_HHMMSS.bak */
    SYSTEMTIME st;
    GetLocalTime(&st);
    char path[MAX_PATH + 64];
    snprintf(path, sizeof(path), "%s\\nbody_%04d%02d%02d_%02d%02d%02d.bak",
             outdir,
             st.wYear, st.wMonth,  st.wDay,
             st.wHour, st.wMinute, st.wSecond);

    if (nbody_save(path))
    {
        print_notice("[BACKUP] %02d:%02d:%02d  Saved: %s",
                     st.wHour, st.wMinute, st.wSecond, path);
    }
    else
    {
        print_notice("[BACKUP] ERROR: could not save %s", path);
    }
}

/* -----------------------------------------------------------------------
 * main
 * --------------------------------------------------------------------- */
int main(int argc, char **argv)
{
    config_t cfg;
    config_defaults(&cfg);
    if (!parse_args(argc, argv, &cfg)) return 1;

    /* --- Print startup info --- */
    printf("=== N-Body Simulation ===\n");

    if (cfg.load_path[0])
    {
        printf("Loading backup: %s\n", cfg.load_path);
        printf("outdir=%s  mode=%s\n",
               cfg.outdir, cfg.use_3d ? "3D" : "2D");
    }
    else
    {
        printf("N=%-8d  steps/frame=%-4d  wrapped=%d\n",
               cfg.N, cfg.steps_per_render, cfg.wrapped);
        printf("dt=%.2e  G=%.2f  soft=%.2e  tile=%d  expansion=%.2f  seed=%llu\n",
               cfg.dt, cfg.g, cfg.soft, cfg.tile, cfg.expansion, cfg.randseed);
        printf("outdir=%s\ncl=%s\nmode=%s\n",
               cfg.outdir, cfg.clpath, cfg.use_3d ? "3D" : "2D");
    }

    print_keyboard_help();

    /* --- Initialise GDI+ --- */
    pngsave_init();

    /* --- Initialise simulation --- */
    if (cfg.load_path[0])
    {
        /* Restore from backup — nbody_set_params + nbody_init are called
         * internally by nbody_load_and_init */
        if (!nbody_load_and_init(cfg.load_path))
        {
            fprintf(stderr, "Failed to load backup '%s'.\n", cfg.load_path);
            pngsave_shutdown();
            return 1;
        }
        /* steps_per_render still comes from command line (or default) */
    }
    else
    {
        nbody_set_params(cfg.N, cfg.tile,
                         (float)cfg.g, (float)cfg.dt, (float)cfg.soft,
                         cfg.wrapped, (float)cfg.expansion,
                         cfg.randseed, cfg.clpath);
        nbody_init();
    }

    /* --- Initialise renderer --- */
    render_state_t rs;
    render_init(&rs, nbody_N, nbody_iswrapped, cfg.outdir, cfg.use_3d);

    /* --- Create window --- */
    HINSTANCE hInst = GetModuleHandle(NULL);
    window_state_t ws;
    HWND hwnd = window_create(&ws, &rs, hInst);
    if (!hwnd)
    {
        fprintf(stderr, "Window creation failed.\n");
        nbody_dispose(); render_free(&rs); pngsave_shutdown(); return 1;
    }

    /* --- Warmup --- */
    printf("Warming up GPU...\n");
    nbody_run(1);
    rs.sim_scale    = nbody_scale;
    rs.sim_timestep = nbody_timestep;
    render_frame(&rs, hwnd, nbody_x_f, nbody_y_f, nbody_z_f, 0, 0.0);
    window_pump(&ws);
    printf("Running.\n");

    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);

    while (!ws.quit)
    {
        /* --- Backup request (B key) --- */
        if (ws.do_backup)
        {
            ws.do_backup = 0;
            do_backup(cfg.outdir);
        }

        if (ws.paused)
        {
            rs.sim_scale    = nbody_scale;
            rs.sim_timestep = nbody_timestep;
            render_frame(&rs, hwnd, nbody_x_f, nbody_y_f, nbody_z_f,
                         0, 0.0);
            window_pump(&ws);
            Sleep(50);
            continue;
        }

        QueryPerformanceCounter(&t0);
        nbody_run(cfg.steps_per_render);
        QueryPerformanceCounter(&t1);

        double elapsed_ms = (double)(t1.QuadPart - t0.QuadPart)
                          / (double)freq.QuadPart * 1000.0;

        rs.sim_scale    = nbody_scale;
        rs.sim_timestep = nbody_timestep;
        render_frame(&rs, hwnd, nbody_x_f, nbody_y_f, nbody_z_f,
                     rs.save_png, elapsed_ms);

        print_status("step=%-8d  scale=%.4f  %.1f ms  |  %s",
                     nbody_timestep, nbody_scale, elapsed_ms,
                     rs.png_last_info);

        window_pump(&ws);
    }

    printf("\nShutting down...\n");
    nbody_dispose();
    render_free(&rs);
    pngsave_shutdown();
    return 0;
}
