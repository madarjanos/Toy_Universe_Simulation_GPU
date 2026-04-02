/*
 * nbody_save.c
 *
 * Binary save / load of the complete N-body simulation state.
 *
 * Created by Claude AI (claude.ai) based on an original C# implementation.
 *
 * The checksum is computed by accumulating all bytes as they are written,
 * so there is no need to rewind and re-read the file for verification.
 * On load, the checksum is verified by re-reading the file up to (but not
 * including) the final 4-byte checksum field.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "nbody_save.h"
#include "nbody_simulation.h"

#define SAVE_MAGIC    0x4E424F44u   /* "NBOD" little-endian */
#define SAVE_VERSION  2u            /* v2: added rand_seed field after expansion_factor */
#define CL_PATH_LEN   256

/* -----------------------------------------------------------------------
 * Checksum-accumulating write helpers
 * --------------------------------------------------------------------- */
static uint32_t s_csum;

static void csum_reset(void) { s_csum = 0; }

static void csum_write(FILE *f, const void *buf, size_t n)
{
    fwrite(buf, 1, n, f);
    const uint8_t *p = (const uint8_t *)buf;
    for (size_t i = 0; i < n; i++) s_csum += p[i];
}

#define CWR(f, val)  csum_write(f, &(val), sizeof(val))

/* -----------------------------------------------------------------------
 * nbody_save
 * --------------------------------------------------------------------- */
int nbody_save(const char *path)
{
    /* Download all 7 arrays from GPU to the *_f host arrays */
    if (!nbody_download_full())
    {
        fprintf(stderr, "nbody_save: GPU download failed\n");
        return 0;
    }

    FILE *f = fopen(path, "wb");
    if (!f)
    {
        fprintf(stderr, "nbody_save: cannot open '%s'\n", path);
        return 0;
    }

    csum_reset();

    /* --- Magic + version --- */
    uint32_t magic   = SAVE_MAGIC;
    uint32_t version = SAVE_VERSION;
    CWR(f, magic);
    CWR(f, version);

    /* --- Parameters --- */
    nbody_params_t p;
    nbody_get_params(&p);

    int32_t N_i   = (int32_t)p.N;
    int32_t tile  = (int32_t)p.tile_size;
    int32_t wrap  = (int32_t)p.wrapped;
    CWR(f, N_i);
    CWR(f, tile);
    CWR(f, wrap);
    CWR(f, p.g_const);
    CWR(f, p.dt);
    CWR(f, p.softening);
    CWR(f, p.expansion_factor);
    CWR(f, p.rand_seed);

    char cl_buf[CL_PATH_LEN] = {0};
    strncpy(cl_buf, p.cl_path, CL_PATH_LEN - 1);
    csum_write(f, cl_buf, CL_PATH_LEN);

    /* --- State scalars --- */
    double  time_d  = nbody_time;
    double  scale_d = nbody_scale;
    int32_t ts      = (int32_t)nbody_timestep;
    int32_t pad     = 0;
    CWR(f, time_d);
    CWR(f, scale_d);
    CWR(f, ts);
    CWR(f, pad);

    /* --- Particle arrays (float32, 7 × N) --- */
    csum_write(f, nbody_x_f,    (size_t)p.N * sizeof(float));
    csum_write(f, nbody_y_f,    (size_t)p.N * sizeof(float));
    csum_write(f, nbody_z_f,    (size_t)p.N * sizeof(float));
    csum_write(f, nbody_vx_f,   (size_t)p.N * sizeof(float));
    csum_write(f, nbody_vy_f,   (size_t)p.N * sizeof(float));
    csum_write(f, nbody_vz_f,   (size_t)p.N * sizeof(float));
    csum_write(f, nbody_mass_f, (size_t)p.N * sizeof(float));

    /* --- Checksum (NOT included in its own sum) --- */
    fwrite(&s_csum, sizeof(s_csum), 1, f);

    fclose(f);
    return 1;
}

/* -----------------------------------------------------------------------
 * nbody_load_and_init
 * --------------------------------------------------------------------- */
int nbody_load_and_init(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        fprintf(stderr, "nbody_load: cannot open '%s'\n", path);
        return 0;
    }

    /* Compute checksum of everything except the last 4 bytes */
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    if (file_size < 4)
    {
        fprintf(stderr, "nbody_load: file too small\n");
        fclose(f); return 0;
    }
    rewind(f);

    uint32_t csum_calc = 0;
    long data_len = file_size - 4;
    for (long i = 0; i < data_len; i++)
    {
        int c = fgetc(f);
        if (c == EOF) { fprintf(stderr, "nbody_load: unexpected EOF\n"); fclose(f); return 0; }
        csum_calc += (uint8_t)c;
    }
    uint32_t csum_stored = 0;
    if (fread(&csum_stored, 4, 1, f) != 1)
    {
        fprintf(stderr, "nbody_load: cannot read checksum\n");
        fclose(f); return 0;
    }
    if (csum_calc != csum_stored)
    {
        fprintf(stderr, "nbody_load: checksum mismatch (got %08X, expected %08X)\n",
                csum_calc, csum_stored);
        fclose(f); return 0;
    }

    /* Re-read from start */
    rewind(f);

    /* Magic + version */
    uint32_t magic = 0, version = 0;
    if (fread(&magic,   4, 1, f) != 1 || magic != SAVE_MAGIC)
    { fprintf(stderr, "nbody_load: bad magic\n"); fclose(f); return 0; }
    if (fread(&version, 4, 1, f) != 1 || version != SAVE_VERSION)
    { fprintf(stderr, "nbody_load: unsupported version %u\n", version); fclose(f); return 0; }

    /* Parameters */
    int32_t N_i, tile, wrap;
    float   g, dt, soft, exp_f;
    unsigned long long seed;
    char    cl_buf[CL_PATH_LEN];

#define RD(var) (fread(&(var), sizeof(var), 1, f) == 1)
    if (!RD(N_i)||!RD(tile)||!RD(wrap)||!RD(g)||!RD(dt)||!RD(soft)||!RD(exp_f)||!RD(seed))
    { fprintf(stderr, "nbody_load: read error (params)\n"); fclose(f); return 0; }
#undef RD
    if (fread(cl_buf, 1, CL_PATH_LEN, f) != CL_PATH_LEN)
    { fprintf(stderr, "nbody_load: read error (cl_path)\n"); fclose(f); return 0; }
    cl_buf[CL_PATH_LEN - 1] = '\0';

    /* State scalars */
    double  time_d, scale_d;
    int32_t ts, pad;
#define RD(var) (fread(&(var), sizeof(var), 1, f) == 1)
    if (!RD(time_d)||!RD(scale_d)||!RD(ts)||!RD(pad))
    { fprintf(stderr, "nbody_load: read error (scalars)\n"); fclose(f); return 0; }
#undef RD

    int N = (int)N_i;

    /* Particle arrays */
    float *fx    = (float *)malloc(N * sizeof(float));
    float *fy    = (float *)malloc(N * sizeof(float));
    float *fz    = (float *)malloc(N * sizeof(float));
    float *fvx   = (float *)malloc(N * sizeof(float));
    float *fvy   = (float *)malloc(N * sizeof(float));
    float *fvz   = (float *)malloc(N * sizeof(float));
    float *fmass = (float *)malloc(N * sizeof(float));

    if (!fx||!fy||!fz||!fvx||!fvy||!fvz||!fmass)
    { fprintf(stderr, "nbody_load: out of memory\n"); fclose(f); return 0; }

    int ok = 1;
    ok &= (fread(fx,    sizeof(float), N, f) == (size_t)N);
    ok &= (fread(fy,    sizeof(float), N, f) == (size_t)N);
    ok &= (fread(fz,    sizeof(float), N, f) == (size_t)N);
    ok &= (fread(fvx,   sizeof(float), N, f) == (size_t)N);
    ok &= (fread(fvy,   sizeof(float), N, f) == (size_t)N);
    ok &= (fread(fvz,   sizeof(float), N, f) == (size_t)N);
    ok &= (fread(fmass, sizeof(float), N, f) == (size_t)N);
    fclose(f);

    if (!ok)
    {
        fprintf(stderr, "nbody_load: read error (particle arrays)\n");
        free(fx); free(fy); free(fz);
        free(fvx); free(fvy); free(fvz); free(fmass);
        return 0;
    }

    /* Apply and initialise */
    nbody_set_params(N, (int)tile, g, dt, soft, (int)wrap, exp_f, seed, cl_buf);
    nbody_init_from_arrays(fx, fy, fz, fvx, fvy, fvz, fmass,
                           time_d, scale_d, (int)ts);

    free(fx); free(fy); free(fz);
    free(fvx); free(fvy); free(fvz); free(fmass);
    return 1;
}
