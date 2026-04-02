/*
 * nbody_kernels.cl
 *
 * OpenCL kernels for the N-body leapfrog integrator (float32).
 *
 * Created by Claude AI (claude.ai) based on an original C# implementation.
 *
 * All three kernels take 12 arguments:
 *
 *   0  __global float* X
 *   1  __global float* Y
 *   2  __global float* Z
 *   3  __global float* VX
 *   4  __global float* VY
 *   5  __global float* VZ
 *   6  __global float* AX
 *   7  __global float* AY
 *   8  __global float* AZ
 *   9  __global float* Mass
 *   10 int   N
 *   11 float scale       (current expansion scale factor)
 *
 * Compile-time constants via -D flags:
 *   -D N=10000  -D TILE_SIZE=256
 *   -D G_CONST=1.0  -D DT=2e-6  -D SOFTENING=1e-3
 *   -D WRAPPED=1              (omit or 0 for non-periodic universe)
 *   -D EXPANSION_FACTOR=20.0
 *
 * Cosmological expansion model:
 *   scale starts at 1.0 and grows by EXPANSION_FACTOR * DT each step (host side).
 *   In kernel 1, after the half-kick and position drift, the peculiar velocity
 *   is multiplied by (scale / (scale + EXPANSION_FACTOR*DT))^2.
 *   This models the physical decrease of peculiar velocities as the universe
 *   expands: in comoving coordinates, v_pec ∝ 1/a (scale factor).
 *   The squared factor accounts for two effects:
 *     1. The coordinate system scales → velocities must be rescaled.
 *     2. Physical peculiar velocity decrease due to expansion (real effect).
 *
 * No cl_khr_fp64 extension needed — everything is float32.
 * Use -cl-fast-relaxed-math for best GPU performance.
 */

/* Minimum-image displacement for the Pac-Man periodic universe */
#if WRAPPED
#define WDIFF(d)  ((d) - floor((d) + 0.5f))
#else
#define WDIFF(d)  (d)
#endif

/* -----------------------------------------------------------------------
 * Kernel 1: half-kick + drift + expansion velocity decay
 *
 *   vel  += 0.5 * DT * acc
 *   pos  += DT * vel         (then wrap if WRAPPED)
 *   vel  *= (scale / (scale + EXPANSION_FACTOR*DT))^2
 *
 * The velocity decay is applied AFTER the position update so the drift
 * uses the pre-expansion velocity (consistent with the leapfrog scheme).
 * --------------------------------------------------------------------- */
__kernel void nbody_half_kick_and_drift_and_expansion(
    __global float* X,   __global float* Y,   __global float* Z,
    __global float* VX,  __global float* VY,  __global float* VZ,
    __global float* AX,  __global float* AY,  __global float* AZ,
    __global float* Mass,
    int n,
    float scale)
{
    int i = get_global_id(0);
    if (i >= n) return;

    const float half_dt = 0.5f * DT;

    /* Half-kick */
    float vx = VX[i] + half_dt * AX[i];
    float vy = VY[i] + half_dt * AY[i];
    float vz = VZ[i] + half_dt * AZ[i];

    /* Drift */
    float nx = X[i] + DT * vx;
    float ny = Y[i] + DT * vy;
    float nz = Z[i] + DT * vz;

#if WRAPPED
    nx -= floor(nx);
    ny -= floor(ny);
    nz -= floor(nz);
#endif

    X[i] = nx;  Y[i] = ny;  Z[i] = nz;

    /* Expansion velocity decay.
     * scale_decr = scale / (scale + EXPANSION_FACTOR * DT)
     * We apply scale_decr^2 to account for both the coordinate rescaling
     * and the physical peculiar velocity decrease. */
    float scale_decr  = scale / (scale + (float)EXPANSION_FACTOR * DT);
    float scale_decr2 = scale_decr * scale_decr;
    VX[i] = vx * scale_decr2;
    VY[i] = vy * scale_decr2;
    VZ[i] = vz * scale_decr2;
}

/* -----------------------------------------------------------------------
 * Kernel 2: force / acceleration computation  (tiled shared memory)
 *
 * One work-item per particle i; inner loop j = 0..N-1.
 * __local tiles are declared inside the kernel (TILE_SIZE is compile-time).
 * native_rsqrt() gives a single-instruction reciprocal sqrt (~23-bit).
 * --------------------------------------------------------------------- */
__kernel __attribute__((reqd_work_group_size(TILE_SIZE, 1, 1)))
void nbody_compute_forces(
    __global float* X,   __global float* Y,   __global float* Z,
    __global float* VX,  __global float* VY,  __global float* VZ,
    __global float* AX,  __global float* AY,  __global float* AZ,
    __global float* Mass,
    int n,
    float scale)
{
    __local float lX[TILE_SIZE];
    __local float lY[TILE_SIZE];
    __local float lZ[TILE_SIZE];
    __local float lM[TILE_SIZE];

    int i   = get_global_id(0);
    int lid = get_local_id(0);

    float xi = (i < n) ? X[i]    : 0.0f;
    float yi = (i < n) ? Y[i]    : 0.0f;
    float zi = (i < n) ? Z[i]    : 0.0f;

    float ax = 0.0f, ay = 0.0f, az = 0.0f;

    float eps2_scaled = SOFTENING * SOFTENING / (scale*scale);
    float G_scaled = G_CONST / (scale*scale);

#if WRAPPED
    const float grav_r2 = 0.25f;    /* (0.5)^2 — max gravitational range */
#else
    const float grav_r2 = 100.0f;   /* (10.0)^2 */
#endif

    int n_tile = (n + TILE_SIZE - 1) / TILE_SIZE;

    for (int tile = 0; tile < n_tile; tile++)
    {
        int j = tile * TILE_SIZE + lid;
        lX[lid] = (j < n) ? X[j]    : 0.0f;
        lY[lid] = (j < n) ? Y[j]    : 0.0f;
        lZ[lid] = (j < n) ? Z[j]    : 0.0f;
        lM[lid] = (j < n) ? Mass[j] : 0.0f;

        barrier(CLK_LOCAL_MEM_FENCE);

        for (int k = 0; k < TILE_SIZE; k++)
        {
            int jj = tile * TILE_SIZE + k;
            if (jj >= n || jj == i) continue;

            float dx = WDIFF(lX[k] - xi);
            float dy = WDIFF(lY[k] - yi);
            float dz = WDIFF(lZ[k] - zi);

            float dist2 = dx*dx + dy*dy + dz*dz + eps2_scaled;
            if (dist2 > grav_r2) continue;

            float invDist3 = native_rsqrt(dist2) / dist2;
            float f = G_scaled * lM[k] * invDist3;

            ax += f * dx;
            ay += f * dy;
            az += f * dz;
        }

        barrier(CLK_LOCAL_MEM_FENCE);
    }

    if (i < n) { AX[i] = ax;  AY[i] = ay;  AZ[i] = az; }
}

/* -----------------------------------------------------------------------
 * Kernel 3: second half-kick velocities
 *
 *   vel += 0.5 * DT * acc
 *
 * scale is accepted as arg 11 for a uniform calling convention with
 * kernels 1 and 2, but is not used here (the expansion decay is in kernel 1).
 * --------------------------------------------------------------------- */
__kernel void nbody_second_half_kick(
    __global float* X,   __global float* Y,   __global float* Z,
    __global float* VX,  __global float* VY,  __global float* VZ,
    __global float* AX,  __global float* AY,  __global float* AZ,
    __global float* Mass,
    int n,
    float scale)
{
    int i = get_global_id(0);
    if (i >= n) return;

    const float half_dt = 0.5f * DT;
    VX[i] += half_dt * AX[i];
    VY[i] += half_dt * AY[i];
    VZ[i] += half_dt * AZ[i];
}
