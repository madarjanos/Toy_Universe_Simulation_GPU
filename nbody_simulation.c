/*
 * nbody_simulation.c
 *
 * OpenCL / GPU N-body gravitational simulation for 64-bit Windows (GCC).
 *
 * Created by Claude AI (claude.ai) based on an original C# implementation.
 *
 * Acceleration arrays (ax/ay/az) are now included in nbody_download_full()
 * and nbody_init_from_arrays() so that a backup/restore cycle preserves the
 * full leapfrog state — without the accelerations the first half-kick after
 * a restore would use zeros and produce incorrect results.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define CL_TARGET_OPENCL_VERSION 120
#include <CL/cl.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nbody_simulation.h"

/* -----------------------------------------------------------------------
 * Runtime parameters
 * --------------------------------------------------------------------- */
static int                s_N         = 10000;
static int                s_TILE      = 256;
static float              s_G         = 1.0f;
static float              s_DT        = 2e-6f;
static float              s_SOFT      = 1e-3f;
static int                s_WRAPPED   = 1;
static float              s_EXPANSION = 20.0f;
static unsigned long long s_SEED      = 301;
static char               s_CL_PATH[MAX_PATH] = "nbody_kernels.cl";

static float s_scale = 1.0f;

void nbody_set_params(int N, int tile_size, float g_const, float dt,
                      float softening, int wrapped, float expansion_factor,
                      unsigned long long rand_seed, const char *cl_path)
{
    s_N         = N;
    s_TILE      = tile_size;
    s_G         = g_const;
    s_DT        = dt;
    s_SOFT      = softening;
    s_WRAPPED   = wrapped;
    s_EXPANSION = expansion_factor;
    s_SEED      = rand_seed;
    if (cl_path) strncpy(s_CL_PATH, cl_path, MAX_PATH - 1);
}

void nbody_get_params(nbody_params_t *out)
{
    out->N                = s_N;
    out->tile_size        = s_TILE;
    out->g_const          = s_G;
    out->dt               = s_DT;
    out->softening        = s_SOFT;
    out->wrapped          = s_WRAPPED;
    out->expansion_factor = s_EXPANSION;
    out->rand_seed        = s_SEED;
    strncpy(out->cl_path, s_CL_PATH, sizeof(out->cl_path) - 1);
    out->cl_path[sizeof(out->cl_path) - 1] = '\0';
}

/* -----------------------------------------------------------------------
 * Public state
 * --------------------------------------------------------------------- */
int    nbody_N         = 0;
double nbody_time      = 0.0;
int    nbody_timestep  = 0;
double nbody_scale     = 1.0;
int    nbody_iswrapped = 0;

float *nbody_x_f    = NULL;
float *nbody_y_f    = NULL;
float *nbody_z_f    = NULL;
float *nbody_vx_f   = NULL;
float *nbody_vy_f   = NULL;
float *nbody_vz_f   = NULL;
float *nbody_mass_f = NULL;
float *nbody_ax_f   = NULL;
float *nbody_ay_f   = NULL;
float *nbody_az_f   = NULL;

double *nbody_x    = NULL;
double *nbody_y    = NULL;
double *nbody_z    = NULL;
double *nbody_vx   = NULL;
double *nbody_vy   = NULL;
double *nbody_vz   = NULL;
double *nbody_mass = NULL;

/* -----------------------------------------------------------------------
 * Private OpenCL state
 * --------------------------------------------------------------------- */
static cl_platform_id   s_platform;
static cl_device_id     s_device;
static cl_context       s_ctx;
static cl_command_queue s_queue;
static cl_program       s_program;

static cl_kernel s_k_half_kick;
static cl_kernel s_k_force;
static cl_kernel s_k_second_kick;

static cl_mem s_d_x,    s_d_y,    s_d_z;
static cl_mem s_d_vx,   s_d_vy,   s_d_vz;
static cl_mem s_d_mass;
static cl_mem s_d_ax,   s_d_ay,   s_d_az;

/* -----------------------------------------------------------------------
 * Splitmix64 PRNG
 * --------------------------------------------------------------------- */
static unsigned long long s_rng;

static void rng_seed(unsigned long long seed)
{
    s_rng = seed ^ 0x123456789ABCDEFULL;
}

static double rng_next(void)
{
    s_rng += 0x9E3779B97F4A7C15ULL;
    unsigned long long z = s_rng;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z = z ^ (z >> 31);
    return (double)(z >> 11) * (1.0 / (double)(1ULL << 53));
}

/* -----------------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------------- */
static void cl_check(cl_int err, const char *where)
{
    if (err != CL_SUCCESS)
    { fprintf(stderr, "OpenCL error %d at: %s\n", (int)err, where); exit(1); }
}

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Cannot open: %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END); *len = (size_t)ftell(f); rewind(f);
    char *buf = (char *)malloc(*len + 1);
    fread(buf, 1, *len, f); buf[*len] = '\0';
    fclose(f);
    return buf;
}

static cl_float *d2f(const double *src, int n)
{
    cl_float *dst = (cl_float *)malloc((size_t)n * sizeof(cl_float));
    for (int i = 0; i < n; i++) dst[i] = (cl_float)src[i];
    return dst;
}

/* -----------------------------------------------------------------------
 * alloc_host_float_arrays — allocate all 10 host float arrays
 * --------------------------------------------------------------------- */
static void alloc_host_float_arrays(int N)
{
    free(nbody_x_f);    free(nbody_y_f);    free(nbody_z_f);
    free(nbody_vx_f);   free(nbody_vy_f);   free(nbody_vz_f);
    free(nbody_mass_f);
    free(nbody_ax_f);   free(nbody_ay_f);   free(nbody_az_f);

    nbody_x_f    = (float *)malloc(N * sizeof(float));
    nbody_y_f    = (float *)malloc(N * sizeof(float));
    nbody_z_f    = (float *)malloc(N * sizeof(float));
    nbody_vx_f   = (float *)malloc(N * sizeof(float));
    nbody_vy_f   = (float *)malloc(N * sizeof(float));
    nbody_vz_f   = (float *)malloc(N * sizeof(float));
    nbody_mass_f = (float *)malloc(N * sizeof(float));
    nbody_ax_f   = (float *)malloc(N * sizeof(float));
    nbody_ay_f   = (float *)malloc(N * sizeof(float));
    nbody_az_f   = (float *)malloc(N * sizeof(float));

    if (!nbody_x_f||!nbody_y_f||!nbody_z_f||
        !nbody_vx_f||!nbody_vy_f||!nbody_vz_f||!nbody_mass_f||
        !nbody_ax_f||!nbody_ay_f||!nbody_az_f)
    { fprintf(stderr, "alloc_host_float_arrays: out of memory\n"); exit(1); }
}

/* -----------------------------------------------------------------------
 * opencl_init — device selection, context, queue, kernel compilation
 * --------------------------------------------------------------------- */
static void opencl_init(int N)
{
    cl_int err;

    cl_uint np = 0;
    clGetPlatformIDs(0, NULL, &np);
    if (!np) { fprintf(stderr, "No OpenCL platform.\n"); exit(1); }

    cl_platform_id *plats = (cl_platform_id *)malloc(np * sizeof(*plats));
    clGetPlatformIDs(np, plats, NULL);

    s_device = NULL; s_platform = plats[0];
    for (cl_uint p = 0; p < np && !s_device; p++)
    {
        cl_uint nd = 0;
        clGetDeviceIDs(plats[p], CL_DEVICE_TYPE_GPU, 0, NULL, &nd);
        if (nd)
        {
            clGetDeviceIDs(plats[p], CL_DEVICE_TYPE_GPU, 1, &s_device, NULL);
            s_platform = plats[p];
        }
    }
    if (!s_device)
    {
        cl_uint nd = 0;
        clGetDeviceIDs(s_platform, CL_DEVICE_TYPE_ALL, 0, NULL, &nd);
        if (!nd) { fprintf(stderr, "No OpenCL device.\n"); exit(1); }
        clGetDeviceIDs(s_platform, CL_DEVICE_TYPE_ALL, 1, &s_device, NULL);
    }
    free(plats);

    char devname[256] = {0};
    clGetDeviceInfo(s_device, CL_DEVICE_NAME, sizeof(devname), devname, NULL);
    printf("GPU: %s\n", devname);

    s_ctx   = clCreateContext(NULL, 1, &s_device, NULL, NULL, &err);
    cl_check(err, "ctx");
    s_queue = clCreateCommandQueue(s_ctx, s_device, 0, &err);
    cl_check(err, "queue");

    size_t src_len = 0;
    char *src = read_file(s_CL_PATH, &src_len);
    s_program = clCreateProgramWithSource(s_ctx, 1,
                    (const char **)&src, &src_len, &err);
    free(src);
    cl_check(err, "createProgram");

    char opts[1024];
    snprintf(opts, sizeof(opts),
        "-DN=%d -DTILE_SIZE=%d "
        "-DG_CONST=%.9g -DDT=%.9g -DSOFTENING=%.9g "
        "-DWRAPPED=%d -DEXPANSION_FACTOR=%.9g "
        "-cl-fast-relaxed-math",
        N, s_TILE,
        (double)s_G, (double)s_DT, (double)s_SOFT,
        s_WRAPPED, (double)s_EXPANSION);

    err = clBuildProgram(s_program, 1, &s_device, opts, NULL, NULL);
    if (err != CL_SUCCESS)
    {
        size_t lsz = 0;
        clGetProgramBuildInfo(s_program, s_device,
                              CL_PROGRAM_BUILD_LOG, 0, NULL, &lsz);
        char *log = (char *)malloc(lsz + 1);
        clGetProgramBuildInfo(s_program, s_device,
                              CL_PROGRAM_BUILD_LOG, lsz, log, NULL);
        log[lsz] = '\0';
        fprintf(stderr, "Build error:\n%s\n", log);
        free(log); exit(1);
    }

    s_k_half_kick = clCreateKernel(s_program,
                        "nbody_half_kick_and_drift_and_expansion", &err);
    cl_check(err, "kernel half_kick_and_drift_and_expansion");
    s_k_force = clCreateKernel(s_program, "nbody_compute_forces", &err);
    cl_check(err, "kernel nbody_compute_forces");
    s_k_second_kick = clCreateKernel(s_program, "nbody_second_half_kick", &err);
    cl_check(err, "kernel second_half_kick");
}

/* -----------------------------------------------------------------------
 * gpu_upload_and_bind — upload arrays to GPU and set kernel args
 * ax/ay/az are uploaded from separate buffers (may be zeros or loaded).
 * --------------------------------------------------------------------- */
static void gpu_upload_and_bind(int N,
    const cl_float *fx,   const cl_float *fy,   const cl_float *fz,
    const cl_float *fvx,  const cl_float *fvy,  const cl_float *fvz,
    const cl_float *fmass,
    const cl_float *fax,  const cl_float *fay,  const cl_float *faz)
{
    cl_int err;
    size_t fsz = (size_t)N * sizeof(cl_float);

#define MKBUF(var, ptr) \
    (var) = clCreateBuffer(s_ctx, \
                CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR, fsz, (void*)(ptr), &err); \
    cl_check(err, "buf " #var)

    MKBUF(s_d_x,    fx);    MKBUF(s_d_y,    fy);    MKBUF(s_d_z,    fz);
    MKBUF(s_d_vx,   fvx);   MKBUF(s_d_vy,   fvy);   MKBUF(s_d_vz,   fvz);
    MKBUF(s_d_mass, fmass);
    MKBUF(s_d_ax,   fax);   MKBUF(s_d_ay,   fay);   MKBUF(s_d_az,   faz);
#undef MKBUF

    cl_int n_cl = (cl_int)N;

#define SET_BUFS(k) \
    clSetKernelArg(k,  0, sizeof(cl_mem), &s_d_x);    \
    clSetKernelArg(k,  1, sizeof(cl_mem), &s_d_y);    \
    clSetKernelArg(k,  2, sizeof(cl_mem), &s_d_z);    \
    clSetKernelArg(k,  3, sizeof(cl_mem), &s_d_vx);   \
    clSetKernelArg(k,  4, sizeof(cl_mem), &s_d_vy);   \
    clSetKernelArg(k,  5, sizeof(cl_mem), &s_d_vz);   \
    clSetKernelArg(k,  6, sizeof(cl_mem), &s_d_ax);   \
    clSetKernelArg(k,  7, sizeof(cl_mem), &s_d_ay);   \
    clSetKernelArg(k,  8, sizeof(cl_mem), &s_d_az);   \
    clSetKernelArg(k,  9, sizeof(cl_mem), &s_d_mass); \
    clSetKernelArg(k, 10, sizeof(cl_int), &n_cl)

    SET_BUFS(s_k_half_kick);
    SET_BUFS(s_k_force);
    SET_BUFS(s_k_second_kick);
#undef SET_BUFS
}

/* -----------------------------------------------------------------------
 * nbody_init — initialise with random particles
 * --------------------------------------------------------------------- */
void nbody_init(void)
{
    int N = s_N;

    nbody_N         = N;
    nbody_iswrapped = s_WRAPPED;
    nbody_time      = 0.0;
    nbody_timestep  = 0;
    s_scale         = 1.0f;
    nbody_scale     = 1.0;

    alloc_host_float_arrays(N);

    rng_seed(s_SEED);

    double *hx    = (double *)malloc(N * sizeof(double));
    double *hy    = (double *)malloc(N * sizeof(double));
    double *hz    = (double *)malloc(N * sizeof(double));
    double *hvx   = (double *)calloc(N, sizeof(double));
    double *hvy   = (double *)calloc(N, sizeof(double));
    double *hvz   = (double *)calloc(N, sizeof(double));
    double *hmass = (double *)malloc(N * sizeof(double));

    for (int i = 0; i < N; i++)
    {
        double r;
        do {
            hx[i] = rng_next(); hy[i] = rng_next(); hz[i] = rng_next();
            if (s_WRAPPED) break;
            double dx = hx[i]-0.5, dy = hy[i]-0.5, dz = hz[i]-0.5;
            r = dx*dx + dy*dy + dz*dz;
        } while (r >= 0.25);
        hmass[i] = 0.1 + rng_next() * 0.9;
    }

    cl_float *fx    = d2f(hx,    N);
    cl_float *fy    = d2f(hy,    N);
    cl_float *fz    = d2f(hz,    N);
    cl_float *fvx   = d2f(hvx,   N);
    cl_float *fvy   = d2f(hvy,   N);
    cl_float *fvz   = d2f(hvz,   N);
    cl_float *fmass = d2f(hmass, N);
    cl_float *fax   = (cl_float *)calloc(N, sizeof(cl_float));
    cl_float *fay   = (cl_float *)calloc(N, sizeof(cl_float));
    cl_float *faz   = (cl_float *)calloc(N, sizeof(cl_float));

    memcpy(nbody_x_f,    fx,    N * sizeof(float));
    memcpy(nbody_y_f,    fy,    N * sizeof(float));
    memcpy(nbody_z_f,    fz,    N * sizeof(float));
    memcpy(nbody_vx_f,   fvx,   N * sizeof(float));
    memcpy(nbody_vy_f,   fvy,   N * sizeof(float));
    memcpy(nbody_vz_f,   fvz,   N * sizeof(float));
    memcpy(nbody_mass_f, fmass, N * sizeof(float));
    memcpy(nbody_ax_f,   fax,   N * sizeof(float));
    memcpy(nbody_ay_f,   fay,   N * sizeof(float));
    memcpy(nbody_az_f,   faz,   N * sizeof(float));

    free(hx); free(hy); free(hz);
    free(hvx); free(hvy); free(hvz); free(hmass);

    opencl_init(N);
    gpu_upload_and_bind(N, fx, fy, fz, fvx, fvy, fvz, fmass, fax, fay, faz);

    free(fx); free(fy); free(fz);
    free(fvx); free(fvy); free(fvz); free(fmass);
    free(fax); free(fay); free(faz);

    printf("nbody_init: %d particles, tile=%d, expansion=%.2f — ready.\n",
           N, s_TILE, (double)s_EXPANSION);
}

/* -----------------------------------------------------------------------
 * nbody_init_from_arrays — initialise GPU from loaded float data
 * Accelerations are uploaded so the leapfrog state is fully restored.
 * --------------------------------------------------------------------- */
void nbody_init_from_arrays(
    const float *x,   const float *y,   const float *z,
    const float *vx,  const float *vy,  const float *vz,
    const float *mass,
    const float *ax,  const float *ay,  const float *az,
    double time, double scale, int timestep)
{
    int N = s_N;

    nbody_N         = N;
    nbody_iswrapped = s_WRAPPED;
    nbody_time      = time;
    nbody_timestep  = timestep;
    s_scale         = (float)scale;
    nbody_scale     = scale;

    alloc_host_float_arrays(N);

    memcpy(nbody_x_f,    x,    N * sizeof(float));
    memcpy(nbody_y_f,    y,    N * sizeof(float));
    memcpy(nbody_z_f,    z,    N * sizeof(float));
    memcpy(nbody_vx_f,   vx,   N * sizeof(float));
    memcpy(nbody_vy_f,   vy,   N * sizeof(float));
    memcpy(nbody_vz_f,   vz,   N * sizeof(float));
    memcpy(nbody_mass_f, mass, N * sizeof(float));
    memcpy(nbody_ax_f,   ax,   N * sizeof(float));
    memcpy(nbody_ay_f,   ay,   N * sizeof(float));
    memcpy(nbody_az_f,   az,   N * sizeof(float));

    opencl_init(N);
    gpu_upload_and_bind(N,
        (const cl_float *)x,    (const cl_float *)y,    (const cl_float *)z,
        (const cl_float *)vx,   (const cl_float *)vy,   (const cl_float *)vz,
        (const cl_float *)mass,
        (const cl_float *)ax,   (const cl_float *)ay,   (const cl_float *)az);

    printf("nbody_init_from_arrays: %d particles, step=%d, scale=%.4f — ready.\n",
           N, timestep, scale);
}

/* -----------------------------------------------------------------------
 * nbody_download_full — download all 10 arrays from GPU
 * --------------------------------------------------------------------- */
int nbody_download_full(void)
{
    int    N   = s_N;
    size_t fsz = (size_t)N * sizeof(cl_float);
    cl_int err;

#define RBF(dev, host) \
    err = clEnqueueReadBuffer(s_queue, (dev), CL_TRUE, \
              0, fsz, (host), 0, NULL, NULL); \
    if (err != CL_SUCCESS) { \
        fprintf(stderr, "nbody_download_full: read error %d\n", (int)err); \
        return 0; \
    }

    RBF(s_d_x,    nbody_x_f);
    RBF(s_d_y,    nbody_y_f);
    RBF(s_d_z,    nbody_z_f);
    RBF(s_d_vx,   nbody_vx_f);
    RBF(s_d_vy,   nbody_vy_f);
    RBF(s_d_vz,   nbody_vz_f);
    RBF(s_d_mass, nbody_mass_f);
    RBF(s_d_ax,   nbody_ax_f);
    RBF(s_d_ay,   nbody_ay_f);
    RBF(s_d_az,   nbody_az_f);
#undef RBF

    return 1;
}

/* -----------------------------------------------------------------------
 * nbody_run
 * --------------------------------------------------------------------- */
void nbody_run(int steps)
{
    cl_int err;
    int    N   = s_N;
    size_t gsz = ((size_t)(N + s_TILE - 1) / s_TILE) * s_TILE;
    size_t lsz = (size_t)s_TILE;

    for (int step = 0; step < steps; step++)
    {
        cl_float scale_cl = s_scale;
        clSetKernelArg(s_k_half_kick,   11, sizeof(cl_float), &scale_cl);
        clSetKernelArg(s_k_force,       11, sizeof(cl_float), &scale_cl);
        clSetKernelArg(s_k_second_kick, 11, sizeof(cl_float), &scale_cl);

        err = clEnqueueNDRangeKernel(s_queue, s_k_half_kick,
                  1, NULL, &gsz, &lsz, 0, NULL, NULL);
        cl_check(err, "enqueue half_kick_and_drift_and_expansion");

        err = clEnqueueNDRangeKernel(s_queue, s_k_force,
                  1, NULL, &gsz, &lsz, 0, NULL, NULL);
        cl_check(err, "enqueue compute_forces");

        err = clEnqueueNDRangeKernel(s_queue, s_k_second_kick,
                  1, NULL, &gsz, &lsz, 0, NULL, NULL);
        cl_check(err, "enqueue second_kick");

        nbody_time += (double)s_DT;
        nbody_timestep++;
        s_scale    += s_EXPANSION * s_DT;
        nbody_scale = (double)s_scale;
    }

    clFinish(s_queue);

    /* Refresh position arrays for renderer */
    size_t fsz = (size_t)N * sizeof(cl_float);
    cl_int e;
    e = clEnqueueReadBuffer(s_queue, s_d_x, CL_TRUE, 0, fsz, nbody_x_f, 0, NULL, NULL);
    cl_check(e, "readX");
    e = clEnqueueReadBuffer(s_queue, s_d_y, CL_TRUE, 0, fsz, nbody_y_f, 0, NULL, NULL);
    cl_check(e, "readY");
    e = clEnqueueReadBuffer(s_queue, s_d_z, CL_TRUE, 0, fsz, nbody_z_f, 0, NULL, NULL);
    cl_check(e, "readZ");
}

/* -----------------------------------------------------------------------
 * nbody_dispose
 * --------------------------------------------------------------------- */
void nbody_dispose(void)
{
    int N = s_N;
    clFinish(s_queue);

    if (!nbody_download_full())
        fprintf(stderr, "nbody_dispose: warning — download failed\n");

    nbody_x    = (double *)malloc(N * sizeof(double));
    nbody_y    = (double *)malloc(N * sizeof(double));
    nbody_z    = (double *)malloc(N * sizeof(double));
    nbody_vx   = (double *)malloc(N * sizeof(double));
    nbody_vy   = (double *)malloc(N * sizeof(double));
    nbody_vz   = (double *)malloc(N * sizeof(double));
    nbody_mass = (double *)malloc(N * sizeof(double));

    for (int i = 0; i < N; i++)
    {
        nbody_x[i]    = (double)nbody_x_f[i];
        nbody_y[i]    = (double)nbody_y_f[i];
        nbody_z[i]    = (double)nbody_z_f[i];
        nbody_vx[i]   = (double)nbody_vx_f[i];
        nbody_vy[i]   = (double)nbody_vy_f[i];
        nbody_vz[i]   = (double)nbody_vz_f[i];
        nbody_mass[i] = (double)nbody_mass_f[i];
    }

    clReleaseKernel(s_k_half_kick);
    clReleaseKernel(s_k_force);
    clReleaseKernel(s_k_second_kick);
    clReleaseProgram(s_program);

    clReleaseMemObject(s_d_x);    clReleaseMemObject(s_d_y);
    clReleaseMemObject(s_d_z);    clReleaseMemObject(s_d_vx);
    clReleaseMemObject(s_d_vy);   clReleaseMemObject(s_d_vz);
    clReleaseMemObject(s_d_mass);
    clReleaseMemObject(s_d_ax);   clReleaseMemObject(s_d_ay);
    clReleaseMemObject(s_d_az);

    clReleaseCommandQueue(s_queue);
    clReleaseContext(s_ctx);

    free(nbody_x_f);    free(nbody_y_f);    free(nbody_z_f);
    free(nbody_vx_f);   free(nbody_vy_f);   free(nbody_vz_f);
    free(nbody_mass_f);
    free(nbody_ax_f);   free(nbody_ay_f);   free(nbody_az_f);
    nbody_x_f = nbody_y_f = nbody_z_f = NULL;
    nbody_vx_f = nbody_vy_f = nbody_vz_f = nbody_mass_f = NULL;
    nbody_ax_f = nbody_ay_f = nbody_az_f = NULL;
}
