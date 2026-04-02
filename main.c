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
 *   --randseed <int>       Random seed                    (default: 301)
 *   --cl <path>            Path to nbody_kernels.cl       (default: nbody_kernels.cl)
 *
 *   Load / save:
 *   --load <path>          Load a .bak file and resume simulation.
 *                          Simulation parameters above are ignored.
 *                          View settings are restored from the file but
 *                          can be overridden by --2d/--3d if specified.
 *
 *   Render parameters (always active):
 *   --outdir <path>        PNG / backup output folder     (default: .)
 *   --2d                   Start in 2D mode
 *   --3d                   Start in 3D mode               (default)
 *
 *   --help
 *
 * Warmup step correction:
 *   The program always calls nbody_run(1) once before the main loop to
 *   trigger GPU JIT compilation and compute the initial accelerations.
 *   To keep the step counter aligned, the first real batch runs with
 *   (steps_per_render - 1) steps instead of steps_per_render.
 *   Subsequent batches run the full steps_per_render.
 *   This way the step counter after the first visible frame is exactly
 *   steps_per_render (or the loaded timestep + steps_per_render after --load).
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

static void print_notice(const char *fmt, ...)
{
    char buf[640];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

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

    char   load_path[MAX_PATH];

    char   outdir[MAX_PATH];
    int    use_3d;
    int    use_3d_set;   /* 1 if --2d/--3d was explicitly given on command line */
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
    c->use_3d_set       = 0;
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
        "  --randseed <int>    Random seed                  (default: 301)\n"
        "  --cl <path>         OpenCL kernel file           (default: nbody_kernels.cl)\n"
        "\n"
        "Load / resume:\n"
        "  --load <path>       Load a .bak file. Simulation params from file.\n"
        "                      View settings also restored (override with --2d/--3d).\n"
        "\n"
        "Render parameters:\n"
        "  --outdir <dir>      PNG and backup output folder (default: .)\n"
        "  --2d                Start in 2D top-down mode\n"
        "  --3d                Start in 3D perspective mode (default)\n"
        "  --help              Show this message\n",
        exe);
    print_keyboard_help();
}

static int parse_args(int argc, char **argv, config_t *c)
{
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
        else if (!strcmp(argv[i],"--randseed"))  { NEED(); c->randseed=(unsigned long long)strtoull(argv[++i],NULL,10); sim_params_set=1; }
        else if (!strcmp(argv[i],"--cl"))        { NEED(); strncpy(c->clpath, argv[++i],MAX_PATH-1); sim_params_set=1; }
        else if (!strcmp(argv[i],"--load"))      { NEED(); strncpy(c->load_path,argv[++i],MAX_PATH-1); }
        else if (!strcmp(argv[i],"--outdir"))    { NEED(); strncpy(c->outdir,argv[++i],MAX_PATH-1); }
        else if (!strcmp(argv[i],"--2d"))        { c->use_3d = 0; c->use_3d_set = 1; }
        else if (!strcmp(argv[i],"--3d"))        { c->use_3d = 1; c->use_3d_set = 1; }
        else
        { fprintf(stderr,"Unknown: %s\n\n",argv[i]); print_usage(argv[0]); return 0; }
#undef NEED
    }

    if (c->load_path[0] && sim_params_set)
        fprintf(stderr,
            "Warning: simulation parameters are ignored when --load is used.\n");

    return 1;
}

/* -----------------------------------------------------------------------
 * Backup handler
 * --------------------------------------------------------------------- */
static void do_backup(const char *outdir, const render_state_t *rs)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    char path[MAX_PATH + 64];
    snprintf(path, sizeof(path), "%s\\nbody_%04d%02d%02d_%02d%02d%02d.bak",
             outdir,
             st.wYear, st.wMonth,  st.wDay,
             st.wHour, st.wMinute, st.wSecond);

    if (nbody_save(path, rs))
        print_notice("[BACKUP] %02d:%02d:%02d  Saved: %s",
                     st.wHour, st.wMinute, st.wSecond, path);
    else
        print_notice("[BACKUP] ERROR: could not save %s", path);
}

/* -----------------------------------------------------------------------
 * main
 * --------------------------------------------------------------------- */
int main(int argc, char **argv)
{
    config_t cfg;
    config_defaults(&cfg);
    if (!parse_args(argc, argv, &cfg)) return 1;

    printf("=== N-Body Simulation ===\n");

    if (cfg.load_path[0])
    {
        printf("Loading backup: %s\n", cfg.load_path);
        printf("outdir=%s\n", cfg.outdir);
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

    pngsave_init();

    /* --- Initialise renderer first so load can restore view into it --- */
    render_state_t rs;
    /* Temporary init with defaults; will be overwritten by load if needed */
    render_init(&rs, 1 /* N placeholder */, 1, cfg.outdir, cfg.use_3d);

    /* --- Initialise simulation --- */
    if (cfg.load_path[0])
    {
        /* Pass &rs so view settings are restored from file into rs */
        if (!nbody_load_and_init(cfg.load_path, &rs))
        {
            fprintf(stderr, "Failed to load backup '%s'.\n", cfg.load_path);
            render_free(&rs); pngsave_shutdown(); return 1;
        }
        /* If user explicitly specified --2d or --3d, override loaded value */
        if (cfg.use_3d_set)
            rs.use_3d = cfg.use_3d;

        /* Snapshot the view settings loaded from file */
        double  snap_zoom    = rs.zoom;
        double  snap_shx     = rs.shift_x;
        double  snap_shy     = rs.shift_y;
        double  snap_shz     = rs.shift_z;
        int     snap_use3d   = rs.use_3d;
        int     snap_savepng = rs.save_png;
        double  snap_rotz    = rs.rot_z;
        double  snap_rotx    = rs.rot_x;
        double  snap_roty    = rs.rot_y;
        int     snap_axis    = rs.rot_axis;

        /* Reinitialise renderer with correct N (render_init zeroes the struct) */
        render_free(&rs);
        render_init(&rs, nbody_N, nbody_iswrapped, cfg.outdir, snap_use3d);

        /* Restore view settings */
        rs.zoom     = snap_zoom;
        rs.shift_x  = snap_shx;
        rs.shift_y  = snap_shy;
        rs.shift_z  = snap_shz;
        rs.use_3d   = snap_use3d;
        rs.save_png = snap_savepng;
        rs.rot_z    = snap_rotz;
        rs.rot_x    = snap_rotx;
        rs.rot_y    = snap_roty;
        rs.rot_axis = snap_axis;
    }
    else
    {
        nbody_set_params(cfg.N, cfg.tile,
                         (float)cfg.g, (float)cfg.dt, (float)cfg.soft,
                         cfg.wrapped, (float)cfg.expansion,
                         cfg.randseed, cfg.clpath);
        nbody_init();

        /* Reinitialise renderer with correct N */
        render_free(&rs);
        render_init(&rs, nbody_N, nbody_iswrapped, cfg.outdir, cfg.use_3d);
    }

    /* --- Create window --- */
    HINSTANCE hInst = GetModuleHandle(NULL);
    window_state_t ws;
    HWND hwnd = window_create(&ws, &rs, hInst);
    if (!hwnd)
    {
        fprintf(stderr, "Window creation failed.\n");
        nbody_dispose(); render_free(&rs); pngsave_shutdown(); return 1;
    }

    /* --- Warmup: run 1 step to trigger GPU JIT and compute accelerations.
     *
     * For a fresh simulation: the first batch will run (steps_per_render - 1)
     * steps so that after the first visible frame the counter reads exactly
     * steps_per_render.
     *
     * For --load: the accelerations are already correct from the backup,
     * but we still do 1 warmup step to ensure the GPU is fully initialised.
     * The same (steps_per_render - 1) correction applies.
     * --------------------------------------------------------------------- */
    printf("Warming up GPU...\n");
    nbody_run(1);
    rs.sim_scale    = nbody_scale;
    rs.sim_timestep = nbody_timestep;
    render_frame(&rs, hwnd, nbody_x_f, nbody_y_f, nbody_z_f, 0, 0.0);
    window_pump(&ws);
    printf("Running. (step counter offset: -1 applied to first batch)\n");

    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);

    /* First real batch runs one step fewer to compensate for the warmup */
    int first_batch = 1;

    while (!ws.quit)
    {
        if (ws.do_backup)
        {
            ws.do_backup = 0;
            do_backup(cfg.outdir, &rs);
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

        /* Determine step count for this batch */
        int steps = cfg.steps_per_render;
        if (first_batch)
        {
            steps = cfg.steps_per_render - 1;
            first_batch = 0;
        }

        QueryPerformanceCounter(&t0);
        if (steps > 0) nbody_run(steps);
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
