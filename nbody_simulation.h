/*
 * nbody_simulation.h
 *
 * Public API for the OpenCL N-body gravitational simulator.
 *
 * Created by Claude AI (claude.ai) based on an original C# implementation.
 *
 * Normal usage:
 *   1. nbody_set_params(...)    -- set parameters
 *   2. nbody_init()             -- initialise OpenCL, compile kernels, upload
 *   3. nbody_run(steps)         -- run steps on GPU
 *   4. nbody_dispose()          -- free GPU resources, populate double arrays
 *
 * Load from backup:
 *   1. nbody_set_params(...)    -- called internally by nbody_load_and_init
 *   2. nbody_init_from_arrays() -- called internally by nbody_load_and_init
 *   (use nbody_save.h / nbody_load_and_init instead of the above two)
 *
 * Backup support (used by nbody_save.c):
 *   nbody_get_params()      -- read back all runtime parameters
 *   nbody_download_full()   -- download all 7 arrays from GPU to _f arrays
 */

#ifndef NBODY_SIMULATION_H
#define NBODY_SIMULATION_H

#ifdef __cplusplus
extern "C" {
#endif

/* -----------------------------------------------------------------------
 * Parameter bundle — used by nbody_get_params and nbody_save.c
 * --------------------------------------------------------------------- */
typedef struct nbody_params
{
    int                N;
    int                tile_size;
    float              g_const;
    float              dt;
    float              softening;
    int                wrapped;
    float              expansion_factor;
    unsigned long long rand_seed;
    char               cl_path[260];
} nbody_params_t;

/* -----------------------------------------------------------------------
 * Set simulation parameters — must be called before nbody_init().
 * --------------------------------------------------------------------- */
void nbody_set_params(
    int                N,
    int                tile_size,
    float              g_const,
    float              dt,
    float              softening,
    int                wrapped,
    float              expansion_factor,
    unsigned long long rand_seed,
    const char        *cl_path
);

/* Read back the currently active parameters (used by nbody_save). */
void nbody_get_params(nbody_params_t *out);

/* -----------------------------------------------------------------------
 * Public state (read-only from outside)
 * --------------------------------------------------------------------- */
extern int    nbody_N;
extern double nbody_time;
extern int    nbody_timestep;
extern double nbody_scale;
extern int    nbody_iswrapped;

/* Host-side float position arrays — refreshed after every nbody_run(). */
extern float *nbody_x_f;
extern float *nbody_y_f;
extern float *nbody_z_f;

/* Host-side float arrays for ALL 7 quantities — valid after
 * nbody_download_full() (called by nbody_save). */
extern float *nbody_vx_f;
extern float *nbody_vy_f;
extern float *nbody_vz_f;
extern float *nbody_mass_f;

/* Host-side double arrays — populated only after nbody_dispose(). */
extern double *nbody_x;
extern double *nbody_y;
extern double *nbody_z;
extern double *nbody_vx;
extern double *nbody_vy;
extern double *nbody_vz;
extern double *nbody_mass;

/* -----------------------------------------------------------------------
 * Core functions
 * --------------------------------------------------------------------- */
void nbody_init(void);
void nbody_run(int steps);
void nbody_dispose(void);

/* -----------------------------------------------------------------------
 * Backup / restore support
 * --------------------------------------------------------------------- */

/*
 * Download all 7 particle arrays (x,y,z,vx,vy,vz,mass) from GPU to the
 * nbody_*_f host arrays.  Does not disturb the GPU state; simulation can
 * continue after this call.
 * Returns 1 on success, 0 on failure.
 */
int nbody_download_full(void);

/*
 * Initialise the simulation from pre-loaded float arrays instead of random
 * particles.  Called by nbody_load_and_init() in nbody_save.c after
 * nbody_set_params() has been called with the restored parameters.
 * Also restores time, scale and timestep.
 */
void nbody_init_from_arrays(
    const float *x,  const float *y,  const float *z,
    const float *vx, const float *vy, const float *vz,
    const float *mass,
    double time, double scale, int timestep
);

#ifdef __cplusplus
}
#endif

#endif /* NBODY_SIMULATION_H */
