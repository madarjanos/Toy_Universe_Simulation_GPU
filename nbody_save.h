/*
 * nbody_save.h
 *
 * Save and load the complete N-body simulation state to/from a binary file.
 *
 * Created by Claude AI (claude.ai) based on an original C# implementation.
 *
 * File format version 3 (all values little-endian / native x86-64):
 *
 *   Offset  Type      Field
 *   ------  --------  --------------------------------------------------
 *   0       uint32    magic = 0x4E424F44  ("NBOD")
 *   4       uint32    version = 3
 *   8       int32     N
 *   12      int32     tile_size
 *   16      int32     wrapped
 *   20      float32   g_const
 *   24      float32   dt
 *   28      float32   softening
 *   32      float32   expansion_factor
 *   36      uint64    rand_seed
 *   44      char[256] cl_path
 *   300     double    time
 *   308     double    scale
 *   316     int32     timestep
 *   320     int32     _pad
 *   324     float32   X[N]
 *   ...     float32   Y[N]
 *   ...     float32   Z[N]
 *   ...     float32   VX[N]
 *   ...     float32   VY[N]
 *   ...     float32   VZ[N]
 *   ...     float32   Mass[N]
 *   ...     float32   AX[N]        (accelerations for correct leapfrog resume)
 *   ...     float32   AY[N]
 *   ...     float32   AZ[N]
 *   ...     double    view_zoom
 *   ...     double    view_shift_x
 *   ...     double    view_shift_y
 *   ...     double    view_shift_z
 *   ...     int32     view_use_3d
 *   ...     int32     view_save_png
 *   ...     double    view_rot_z
 *   ...     double    view_rot_x
 *   ...     double    view_rot_y
 *   ...     int32     view_rot_axis
 *   ...     int32     _pad2
 *   end     uint32    checksum
 *
 * Total size: 324 + 10*N*4 + 80 + 4 bytes.
 */

#ifndef NBODY_SAVE_H
#define NBODY_SAVE_H

#include "render.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Save the complete simulation state including view settings.
 * rs may be NULL — in that case view settings are not saved (zeroed).
 * Returns 1 on success, 0 on failure.
 */
int nbody_save(const char *path, const render_state_t *rs);

/*
 * Load a saved state, initialise the simulation, and optionally restore
 * the view settings into *rs.
 * rs may be NULL — in that case view settings from the file are ignored.
 * Returns 1 on success, 0 on failure.
 */
int nbody_load_and_init(const char *path, render_state_t *rs);

#ifdef __cplusplus
}
#endif

#endif /* NBODY_SAVE_H */
