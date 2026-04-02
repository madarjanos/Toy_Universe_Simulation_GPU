/*
 * nbody_save.h
 *
 * Save and load the complete N-body simulation state to/from a binary file.
 *
 * Created by Claude AI (claude.ai) based on an original C# implementation.
 *
 * File format (all values little-endian, native byte order on x86-64):
 *
 *   Offset  Type      Field
 *   ------  --------  --------------------------------------------------
 *   0       uint32    magic = 0x4E424F44  ("NBOD")
 *   4       uint32    version = 2
 *   8       int32     N          (particle count)
 *   12      int32     tile_size
 *   16      int32     wrapped
 *   20      float32   g_const
 *   24      float32   dt
 *   28      float32   softening
 *   32      float32   expansion_factor
 *   36      uint64    rand_seed
 *   44      char[256] cl_path    (null-terminated, padded with zeros)
 *   300     double    time       (elapsed simulation time)
 *   308     double    scale      (current expansion scale)
 *   316     int32     timestep
 *   320     int32     _pad       (alignment padding)
 *   324     float32   X[N]       (particle positions and velocities)
 *   324+N*4 float32   Y[N]
 *   ...     float32   Z[N]
 *   ...     float32   VX[N]
 *   ...     float32   VY[N]
 *   ...     float32   VZ[N]
 *   ...     float32   Mass[N]
 *   end     uint32    checksum   (simple sum of all preceding bytes mod 2^32)
 *
 * Total size: 324 + 7*N*4 + 4 bytes.
 *
 * Usage:
 *   Save:  nbody_save(path)        -- call while simulation is running
 *   Load:  nbody_load_and_init(path) -- call instead of nbody_set_params+nbody_init
 */

#ifndef NBODY_SAVE_H
#define NBODY_SAVE_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Save the current simulation state (GPU data is downloaded to host first).
 * Returns 1 on success, 0 on failure (error message printed to stderr).
 * Safe to call while the simulation is running between nbody_run() calls.
 */
int nbody_save(const char *path);

/*
 * Load a previously saved state and initialise the simulation from it.
 * This replaces nbody_set_params() + nbody_init() entirely.
 * Returns 1 on success, 0 on failure (error message printed to stderr).
 * On failure the simulation state is undefined; do not call nbody_run().
 */
int nbody_load_and_init(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* NBODY_SAVE_H */
