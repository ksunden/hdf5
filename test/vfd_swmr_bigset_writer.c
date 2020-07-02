/*
 * Copyright by The HDF Group.
 * Copyright by the Board of Trustees of the University of Illinois.
 * All rights reserved.
 *
 * This file is part of HDF5.  The full HDF5 copyright notice, including
 * terms governing use, modification, and redistribution, is contained in
 * the COPYING file, which can be found at the root of the source code
 * distribution tree, or in https://support.hdfgroup.org/ftp/HDF5/releases.
 * If you do not have access to either file, you may request a copy from
 * help@hdfgroup.org.
 */

#include <err.h>
#include <libgen.h>
#include <time.h> /* nanosleep(2) */
#include <unistd.h> /* getopt(3) */

#define H5C_FRIEND              /*suppress error about including H5Cpkg   */
#define H5F_FRIEND              /*suppress error about including H5Fpkg   */

#include "hdf5.h"

#include "H5Cpkg.h"
#include "H5Fpkg.h"
// #include "H5Iprivate.h"
#include "H5HGprivate.h"
#include "H5VLprivate.h"

#include "testhdf5.h"
#include "vfd_swmr_common.h"

#define ROWS 256
#define COLS 512
#define RANK 2

static const unsigned int hang_back = 3;

typedef struct _base {
    hsize_t row, col;
} base_t;

typedef struct _mat {
    unsigned rows, cols;
    uint32_t elt[1];
} mat_t;

typedef struct _quadrant {
    hsize_t start[RANK];
    hsize_t stride[RANK];
    hsize_t block[RANK];
    hsize_t count[RANK];
    hid_t space;
} quadrant_t;

typedef struct {
	hid_t *dataset;
        hid_t dapl, dcpl, file, filespace, filetype, memspace, one_by_one_sid,
            quadrant_dcpl, quadrant_space;
        unsigned ndatasets;
	char filename[PATH_MAX];
	char progname[PATH_MAX];
	struct timespec update_interval;
        struct {
            quadrant_t ul, ur, bl, br, src;
        } quadrants;
	unsigned int cols, rows;
	unsigned int asteps;
	unsigned int nsteps;
        bool two_dee;
        bool wait_for_signal;
        bool use_vds;
        bool use_vfd_swmr;
        hsize_t chunk_dims[RANK];
        hsize_t one_dee_max_dims[RANK];
} state_t;

#define ALL_HID_INITIALIZER (state_t){					\
	  .memspace = H5I_INVALID_HID					\
	, .dapl = H5I_INVALID_HID					\
	, .dcpl = H5I_INVALID_HID					\
	, .file = H5I_INVALID_HID					\
	, .filespace = H5I_INVALID_HID					\
	, .filetype = H5T_NATIVE_UINT32					\
	, .one_by_one_sid = H5I_INVALID_HID				\
	, .quadrant_dcpl = H5I_INVALID_HID				\
	, .quadrant_space = H5I_INVALID_HID				\
	, .rows = ROWS						        \
	, .cols = COLS						        \
        , .ndatasets = 5                                                \
	, .asteps = 10							\
	, .nsteps = 100							\
	, .filename = ""						\
        , .two_dee = false                                              \
        , .wait_for_signal = true                                       \
        , .use_vds = false                                              \
        , .use_vfd_swmr = true                                          \
        , .one_dee_max_dims = {ROWS, H5S_UNLIMITED}                     \
        , .chunk_dims = {ROWS, COLS}                                    \
	, .update_interval = (struct timespec){				\
		  .tv_sec = 0						\
		, .tv_nsec = 1000000000UL / 30 /* 1/30 second */}}

static void state_init(state_t *, int, char **);

static const hid_t badhid = H5I_INVALID_HID;

static const hsize_t two_dee_max_dims[RANK] = {H5S_UNLIMITED, H5S_UNLIMITED};

static uint32_t
matget(const mat_t *mat, unsigned i, unsigned j)
{
    assert(i < mat->rows && j < mat->cols);

    return mat->elt[i * mat->cols + j];
}

static void
matset(mat_t *mat, unsigned i, unsigned j, uint32_t v)
{
    assert(i < mat->rows && j < mat->cols);

    mat->elt[i * mat->cols + j] = v;
}

static mat_t *
newmat(unsigned rows, unsigned cols)
{
    mat_t *mat;

    mat = malloc(sizeof(*mat) + (rows * cols - 1) * sizeof(mat->elt[0]));

    if (mat == NULL)
        err(EXIT_FAILURE, "%s: malloc", __func__);

    mat->rows = rows;
    mat->cols = cols;

    return mat;
}

static void
usage(const char *progname)
{
	fprintf(stderr, "usage: %s [-S] [-W] [-a steps] [-b] [-c cols]\n"
                "    [-d dims]\n"
                "    [-n iterations] [-r rows] [-s datasets]\n"
                "    [-u milliseconds]\n"
		"\n"
		"-S:	               do not use VFD SWMR\n"
		"-V:	               use virtual datasets\n"
		"-W:	               do not wait for a signal before\n"
                "                      exiting\n"
		"-a steps:	       `steps` between adding attributes\n"
		"-b:	               write data in big-endian byte order\n"
		"-c cols:	       `cols` columns per chunk\n"
		"-d 1|one|2|two|both:  select dataset expansion in one or\n"
                "                      both dimensions\n"
		"-n iterations:        how many times to expand each dataset\n"
		"-r rows:	       `rows` rows per chunk\n"
		"-s datasets:          number of datasets to create\n"
		"-u ms:                milliseconds interval between updates\n"
                "                      to %s.h5\n"
		"\n",
		progname, progname);
	exit(EXIT_FAILURE);
}

static void
make_quadrant_dataspace(state_t *s, quadrant_t *q)
{
    if ((q->space = H5Scopy(s->filespace)) < 0)
        errx(EXIT_FAILURE, "%s: H5Scopy failed", __func__);

    if (H5Sselect_hyperslab(q->space, H5S_SELECT_SET, q->start, q->stride,
            q->count, q->block) < 0)
        errx(EXIT_FAILURE, "%s: H5Sselect_hyperslab failed", __func__);
}

static void
state_init(state_t *s, int argc, char **argv)
{
    unsigned long tmp;
    int ch;
    unsigned i;
    const hsize_t dims = 1;
    char tfile[PATH_MAX];
    char *end;
    unsigned long millis;
    quadrant_t * const ul = &s->quadrants.ul,
               * const ur = &s->quadrants.ur,
               * const bl = &s->quadrants.bl,
               * const br = &s->quadrants.br,
               * const src = &s->quadrants.src;

    *s = ALL_HID_INITIALIZER;
    esnprintf(tfile, sizeof(tfile), "%s", argv[0]);
    esnprintf(s->progname, sizeof(s->progname), "%s", basename(tfile));

    while ((ch = getopt(argc, argv, "SVWa:bc:d:n:qr:s:u:")) != -1) {
        switch (ch) {
        case 'S':
            s->use_vfd_swmr = false;
            break;
        case 'V':
            s->use_vds = true;
            break;
        case 'W':
            s->wait_for_signal = false;
            break;
        case 'd':
            if (strcmp(optarg, "1") == 0 ||
                strcmp(optarg, "one") == 0)
                s->two_dee = false;
            else if (strcmp(optarg, "2") == 0 ||
                     strcmp(optarg, "two") == 0 ||
                     strcmp(optarg, "both") == 0)
                s->two_dee = true;
            else {
                    errx(EXIT_FAILURE,
                        "bad -d argument \"%s\"", optarg);
            }
            break;
        case 'a':
        case 'c':
        case 'n':
        case 'r':
        case 's':
            errno = 0;
            tmp = strtoul(optarg, &end, 0);
            if (end == optarg || *end != '\0') {
                errx(EXIT_FAILURE, "couldn't parse `-%c` argument `%s`", ch,
                    optarg);
            } else if (errno != 0) {
                err(EXIT_FAILURE, "couldn't parse `-%c` argument `%s`", ch,
                    optarg);
            } else if (tmp > UINT_MAX)
                errx(EXIT_FAILURE, "`-%c` argument `%lu` too large", ch, tmp);

            if ((ch == 'c' || ch == 'r') && tmp == 0) {
                errx(EXIT_FAILURE, "`-%c` argument `%lu` must be >= 1", ch,
                    tmp);
            }

            if (ch == 'a')
                s->asteps = (unsigned)tmp;
            else if (ch == 'c')
                s->cols = (unsigned)tmp;
            else if (ch == 'n')
                s->nsteps = (unsigned)tmp;
            else if (ch == 'r')
                s->rows = (unsigned)tmp;
            else
                s->ndatasets = (unsigned)tmp;
            break;
        case 'b':
            s->filetype = H5T_STD_U32BE;
            break;
        case 'q':
            verbosity = 0;
            break;
        case 'u':
            errno = 0;
            millis = strtoul(optarg, &end, 0);
            if (millis == ULONG_MAX && errno == ERANGE) {
                    err(EXIT_FAILURE,
                        "option -p argument \"%s\"", optarg);
            } else if (*end != '\0') {
                    errx(EXIT_FAILURE,
                        "garbage after -p argument \"%s\"", optarg);
            }
            s->update_interval.tv_sec = (time_t)(millis / 1000UL);
            s->update_interval.tv_nsec =
                (long)((millis * 1000000UL) % 1000000000UL);
            dbgf(1, "%lu milliseconds between updates\n", millis);
            break;
        case '?':
        default:
            usage(s->progname);
            break;
        }
    }
    argc -= optind;
    argv += optind;

    if (argc > 0)
        errx(EXIT_FAILURE, "unexpected command-line arguments");

    if (s->use_vds && s->two_dee) {
        errx(EXIT_FAILURE,
            "virtual datasets and 2D datasets are mutually exclusive");
    }

    s->chunk_dims[0] = s->rows;
    s->chunk_dims[1] = s->cols;
    s->one_dee_max_dims[0] = s->rows;
    s->one_dee_max_dims[1] = H5S_UNLIMITED;

    if ((s->dcpl = H5Pcreate(H5P_DATASET_CREATE)) < 0) {
        errx(EXIT_FAILURE, "%s.%d: H5Pcreate failed",
            __func__, __LINE__);
    }

    if (H5Pset_chunk(s->dcpl, RANK, s->chunk_dims) < 0)
        errx(EXIT_FAILURE, "H5Pset_chunk failed");

    s->filespace = H5Screate_simple(NELMTS(s->chunk_dims), s->chunk_dims,
        s->two_dee ? two_dee_max_dims : s->one_dee_max_dims);

    if (s->filespace < 0) {
        errx(EXIT_FAILURE, "%s.%d: H5Screate_simple failed",
                __func__, __LINE__);
    }

    if (s->use_vds) {
        const hsize_t half_chunk_dims[RANK] = {s->rows / 2, s->cols / 2};
        const hsize_t half_max_dims[RANK] = {s->rows / 2, H5S_UNLIMITED};

        if ((s->quadrant_dcpl = H5Pcreate(H5P_DATASET_CREATE)) < 0) {
            errx(EXIT_FAILURE, "%s.%d: H5Pcreate failed",
                __func__, __LINE__);
        }

        if (H5Pset_chunk(s->quadrant_dcpl, RANK, half_chunk_dims) < 0)
            errx(EXIT_FAILURE, "H5Pset_chunk failed");

        *ul = (quadrant_t){
          .start = {0, 0}
        , .stride = {s->rows, s->cols}
        , .block = {s->rows / 2, s->cols / 2}
        , .count = {1, H5S_UNLIMITED}};

        *ur = (quadrant_t){
          .start = {s->rows / 2, 0}
        , .stride = {s->rows, s->cols}
        , .block = {s->rows / 2, s->cols / 2}
        , .count = {1, H5S_UNLIMITED}};

        *bl = (quadrant_t){
          .start = {0, s->cols / 2}
        , .stride = {s->rows, s->cols}
        , .block = {s->rows / 2, s->cols / 2}
        , .count = {1, H5S_UNLIMITED}};

        *br = (quadrant_t){
          .start = {s->rows / 2, s->cols / 2}
        , .stride = {s->rows, s->cols}
        , .block = {s->rows / 2, s->cols / 2}
        , .count = {1, H5S_UNLIMITED}};

        make_quadrant_dataspace(s, ul);
        make_quadrant_dataspace(s, ur);
        make_quadrant_dataspace(s, bl);
        make_quadrant_dataspace(s, br);

        *src = (quadrant_t){
          .start = {0, 0}
        , .stride = {s->rows / 2, s->cols / 2}
        , .block = {s->rows / 2, s->cols / 2}
        , .count = {1, H5S_UNLIMITED}};

        s->quadrant_space = H5Screate_simple(RANK, half_chunk_dims,
            half_max_dims);

        if (s->quadrant_space < 0) {
            errx(EXIT_FAILURE, "%s.%d: H5Screate_simple failed",
                    __func__, __LINE__);
        }

        src->space = H5Scopy(s->quadrant_space);

        if (src->space < 0) {
            errx(EXIT_FAILURE, "%s.%d: H5Screate_simple failed",
                    __func__, __LINE__);
        }

        if (H5Sselect_hyperslab(src->space, H5S_SELECT_SET, src->start,
                src->stride, src->count, src->block) < 0)
            errx(EXIT_FAILURE, "%s: H5Sselect_hyperslab failed", __func__);
    }

    /* space for attributes */
    if ((s->one_by_one_sid = H5Screate_simple(1, &dims, &dims)) < 0)
        errx(EXIT_FAILURE, "H5Screate_simple failed");

    s->dataset = malloc(sizeof(*s->dataset) * s->ndatasets);
    if (s->dataset == NULL)
        err(EXIT_FAILURE, "could not allocate dataset handles");

    for (i = 0; i < s->ndatasets; i++)
        s->dataset[i] = badhid;

    s->memspace = H5Screate_simple(RANK, s->chunk_dims, NULL);

    if (s->memspace < 0) {
            errx(EXIT_FAILURE, "%s.%d: H5Screate_simple failed",
                __func__, __LINE__);
    }

    esnprintf(s->filename, sizeof(s->filename), "vfd_swmr_bigset.h5");
}

static void
state_destroy(state_t *s)
{
    if (H5Pclose(s->dapl) < 0)
        errx(EXIT_FAILURE, "H5Pclose(fapl)");

    s->dapl = badhid;

    if (H5Pclose(s->dcpl) < 0)
        errx(EXIT_FAILURE, "H5Pclose(dcpl)");

    s->dcpl = badhid;

    if (H5Sclose(s->filespace) < 0)
        errx(EXIT_FAILURE, "H5Sclose failed");

    s->filespace = badhid;

    if (s->use_vds && H5Sclose(s->quadrant_space) < 0)
        errx(EXIT_FAILURE, "H5Sclose failed");

    s->quadrant_space = badhid;

    /* TBD destroy spaces belonging to quadrants */

    if (s->use_vds && H5Pclose(s->quadrant_dcpl) < 0)
        errx(EXIT_FAILURE, "H5Pclose(dcpl)");

    s->quadrant_dcpl = badhid;

    if (H5Fclose(s->file) < 0)
        errx(EXIT_FAILURE, "H5Fclose");

    s->file = badhid;
}

static void
create_extensible_dset(state_t *s, unsigned int which)
{
    const quadrant_t * const ul = &s->quadrants.ul,
                     * const ur = &s->quadrants.ur,
                     * const bl = &s->quadrants.bl,
                     * const br = &s->quadrants.br,
                     * const src = &s->quadrants.src;
    char dname[sizeof("/dataset-9999999999")];
    char ul_dname[sizeof("/ul-dataset-9999999999")],
         ur_dname[sizeof("/ur-dataset-9999999999")],
         bl_dname[sizeof("/bl-dataset-9999999999")],
         br_dname[sizeof("/br-dataset-9999999999")];
    hid_t ul_ds, ur_ds, bl_ds, br_ds;
    hid_t dcpl, ds;

    assert(which < s->ndatasets);
    assert(s->dataset[which] == badhid);

    esnprintf(dname, sizeof(dname), "/dataset-%d", which);

    if ((dcpl = H5Pcopy(s->dcpl)) < 0)
        errx(EXIT_FAILURE, "%s: H5Pcopy failed", __func__);

    if (s->use_vds) {

        esnprintf(ul_dname, sizeof(ul_dname), "/ul-dataset-%d", which);
        esnprintf(ur_dname, sizeof(ur_dname), "/ur-dataset-%d", which);
        esnprintf(bl_dname, sizeof(bl_dname), "/bl-dataset-%d", which);
        esnprintf(br_dname, sizeof(br_dname), "/br-dataset-%d", which);

        ul_ds = H5Dcreate2(s->file, ul_dname, s->filetype, s->quadrant_space,
            H5P_DEFAULT, s->quadrant_dcpl, s->dapl);

        if (ul_ds < 0)
            errx(EXIT_FAILURE, "H5Dcreate(, \"%s\", ) failed", ul_dname);

        ur_ds = H5Dcreate2(s->file, ur_dname, s->filetype, s->quadrant_space,
            H5P_DEFAULT, s->quadrant_dcpl, s->dapl);

        if (ur_ds < 0)
            errx(EXIT_FAILURE, "H5Dcreate(, \"%s\", ) failed", ur_dname);

        bl_ds = H5Dcreate2(s->file, bl_dname, s->filetype, s->quadrant_space,
            H5P_DEFAULT, s->quadrant_dcpl, s->dapl);

        if (bl_ds < 0)
            errx(EXIT_FAILURE, "H5Dcreate(, \"%s\", ) failed", bl_dname);

        br_ds = H5Dcreate2(s->file, br_dname, s->filetype, s->quadrant_space,
            H5P_DEFAULT, s->quadrant_dcpl, s->dapl);

        if (br_ds < 0)
            errx(EXIT_FAILURE, "H5Dcreate(, \"%s\", ) failed", br_dname);

        if (H5Dclose(ul_ds) < 0 || H5Dclose(ur_ds) < 0 ||
            H5Dclose(bl_ds) < 0 || H5Dclose(br_ds) < 0)
            errx(EXIT_FAILURE, "H5Dclose failed");

        if (H5Pset_virtual(dcpl, ul->space, s->filename, ul_dname,
                src->space) < 0)
            errx(EXIT_FAILURE, "%s: H5Pset_virtual failed", __func__);

        if (H5Pset_virtual(dcpl, ur->space, s->filename, ur_dname,
                src->space) < 0)
            errx(EXIT_FAILURE, "%s: H5Pset_virtual failed", __func__);

        if (H5Pset_virtual(dcpl, bl->space, s->filename, bl_dname,
                src->space) < 0)
            errx(EXIT_FAILURE, "%s: H5Pset_virtual failed", __func__);

        if (H5Pset_virtual(dcpl, br->space, s->filename, br_dname,
                src->space) < 0)
            errx(EXIT_FAILURE, "%s: H5Pset_virtual failed", __func__);
    }

    ds = H5Dcreate2(s->file, dname, s->filetype, s->filespace,
        H5P_DEFAULT, dcpl, s->dapl);

    if (ds < 0)
        errx(EXIT_FAILURE, "H5Dcreate(, \"%s\", ) failed", dname);

    if (H5Pclose(dcpl) < 0)
        errx(EXIT_FAILURE, "%s: H5Pclose failed", __func__);

    s->dataset[which] = ds;
}

static void
close_extensible_dset(state_t *s, unsigned int which)
{
    char dname[sizeof("/dataset-9999999999")];
    hid_t ds;

    assert(which < s->ndatasets);

    esnprintf(dname, sizeof(dname), "/dataset-%d", which);

    ds = s->dataset[which];

    if (H5Dclose(ds) < 0)
        errx(EXIT_FAILURE, "H5Dclose failed for \"%s\"", dname);

    s->dataset[which] = badhid;
}

static void
open_extensible_dset(state_t *s, unsigned int which)
{
    hsize_t dims[RANK], maxdims[RANK];
    char dname[sizeof("/dataset-9999999999")];
    hid_t ds, filespace, ty;

    assert(which < s->ndatasets);
    assert(s->dataset[which] == badhid);

    esnprintf(dname, sizeof(dname), "/dataset-%d", which);

    ds = H5Dopen(s->file, dname, s->dapl);

    if (ds < 0)
        errx(EXIT_FAILURE, "H5Dopen(, \"%s\", ) failed", dname);

    if ((ty = H5Dget_type(ds)) < 0)
        errx(EXIT_FAILURE, "H5Dget_type failed");

    if (H5Tequal(ty, s->filetype) <= 0)
        errx(EXIT_FAILURE, "Unexpected data type");

    if ((filespace = H5Dget_space(ds)) < 0)
        errx(EXIT_FAILURE, "H5Dget_space failed");

    if (H5Sget_simple_extent_ndims(filespace) != RANK)
        errx(EXIT_FAILURE, "Unexpected rank");

    if (H5Sget_simple_extent_dims(filespace, dims, maxdims) < 0)
        errx(EXIT_FAILURE, "H5Sget_simple_extent_dims failed");

    if (H5Sclose(filespace) < 0)
        errx(EXIT_FAILURE, "H5Sclose failed");

    filespace = badhid;

    if (s->two_dee) {
        if (maxdims[0] != two_dee_max_dims[0] ||
            maxdims[1] != two_dee_max_dims[1] ||
            maxdims[0] != maxdims[1]) {
                errx(EXIT_FAILURE, "Unexpected maximum dimensions %"
                    PRIuHSIZE " x %" PRIuHSIZE, maxdims[0], maxdims[1]);
        }
    } else if (maxdims[0] != s->one_dee_max_dims[0] ||
               maxdims[1] != s->one_dee_max_dims[1] ||
               dims[0] != s->chunk_dims[0]) {
        errx(EXIT_FAILURE, "Unexpected maximum dimensions %"
            PRIuHSIZE " x %" PRIuHSIZE " or columns %" PRIuHSIZE,
            maxdims[0], maxdims[1], dims[1]);
    }

    s->dataset[which] = ds;
}

static void
set_or_verify_matrix(mat_t *mat, unsigned int which, base_t base, bool do_set)
{
    unsigned row, col;

    for (row = 0; row < mat->rows; row++) {
        for (col = 0; col < mat->cols; col++) {
            uint32_t v;
            hsize_t i = base.row + row,
                    j = base.col + col,
                    u;

            if (j <= i)
                u = (i + 1) * (i + 1) - 1 - j;
            else
                u = j * j + i;

            assert(UINT32_MAX - u >= which);
            v = (uint32_t)(u + which);
            if (do_set)
                matset(mat, row, col, v);
            else if (matget(mat, row, col) != v) {
                errx(EXIT_FAILURE, "matrix mismatch "
                    "at %" PRIuHSIZE ", %" PRIuHSIZE " (%u, %u), "
                    "read %" PRIu32 " expecting %" PRIu32,
                    i, j, row, col, matget(mat, row, col), v);
            }
        }
    }
}

static void
init_matrix(mat_t *mat, unsigned int which, base_t base)
{
    set_or_verify_matrix(mat, which, base, true);
}

static void
verify_matrix(mat_t *mat, unsigned int which, base_t base)
{
    set_or_verify_matrix(mat, which, base, false);
}

static void
verify_chunk(state_t *s, hid_t filespace,
    mat_t *mat, unsigned which, base_t base)
{
    hsize_t offset[RANK] = {base.row, base.col};
    herr_t status;
    hid_t ds;

    assert(which < s->ndatasets);

    dbgf(1, "verifying chunk %" PRIuHSIZE ", %" PRIuHSIZE "\n",
        base.row, base.col);

    ds = s->dataset[which];

    status = H5Sselect_hyperslab(filespace, H5S_SELECT_SET, offset,
        NULL, s->chunk_dims, NULL);

    if (status < 0)
        errx(EXIT_FAILURE, "H5Sselect_hyperslab failed");

    status = H5Dread(ds, H5T_NATIVE_UINT32, s->memspace, filespace,
        H5P_DEFAULT, mat->elt);

    if (status < 0)
        errx(EXIT_FAILURE, "H5Dread failed");

    verify_matrix(mat, which, base);
}

static void
init_and_write_chunk(state_t *s, hid_t filespace,
    mat_t *mat, unsigned which, base_t base)
{
    hsize_t offset[RANK] = {base.row, base.col};
    herr_t status;
    hid_t ds;

    assert(which < s->ndatasets);

    ds = s->dataset[which];

    init_matrix(mat, which, base);

    status = H5Sselect_hyperslab(filespace, H5S_SELECT_SET, offset,
        NULL, s->chunk_dims, NULL);

    if (status < 0)
        errx(EXIT_FAILURE, "H5Sselect_hyperslab failed");

    status = H5Dwrite(ds, H5T_NATIVE_UINT32, s->memspace, filespace,
        H5P_DEFAULT, mat->elt);

    if (status < 0)
        errx(EXIT_FAILURE, "H5Dwrite failed");
}

static void
verify_dset_attribute(hid_t ds, unsigned int which, unsigned int step)
{
    unsigned int read_step;
    hid_t aid;
    char name[sizeof("attr-9999999999")];

    esnprintf(name, sizeof(name), "attr-%u", step);

    dbgf(1, "verifying attribute %s on dataset %u equals %u\n", name, which,
        step);

    if ((aid = H5Aopen(ds, name, H5P_DEFAULT)) < 0)
        errx(EXIT_FAILURE, "H5Acreate2 failed");

    if (H5Aread(aid, H5T_NATIVE_UINT, &read_step) < 0)
        errx(EXIT_FAILURE, "H5Aread failed");

    if (H5Aclose(aid) < 0)
        errx(EXIT_FAILURE, "H5Aclose failed");

    if (read_step != step)
        errx(EXIT_FAILURE, "expected %u read %u", step, read_step);
}

static void
verify_extensible_dset(state_t *s, unsigned int which, mat_t *mat,
    unsigned *stepp)
{
    hid_t ds, filespace;
    hsize_t size[RANK];
    base_t base, last;
    unsigned int ncols, last_step, step;

    assert(which < s->ndatasets);

    ds = s->dataset[which];

    if (H5Drefresh(ds) < 0)
        errx(EXIT_FAILURE, "H5Drefresh failed");

    filespace = H5Dget_space(ds);

    if (filespace == badhid)
        errx(EXIT_FAILURE, "H5Dget_space failed");

    if (H5Sget_simple_extent_dims(filespace, size, NULL) < 0)
        errx(EXIT_FAILURE, "H5Sget_simple_extent_dims failed");

    ncols = (unsigned)(size[1] / s->chunk_dims[1]);
    if (ncols < hang_back)
        goto out;

    last_step = ncols - hang_back;

    for (step = *stepp; step <= last_step; step++) {
        const unsigned ofs = step % 2;

        dbgf(1, "%s: which %u step %u\n", __func__, which, step);

        if (s->two_dee) {
            size[0] = s->chunk_dims[0] * (1 + step);
            size[1] = s->chunk_dims[1] * (1 + step);
            last.row = s->chunk_dims[0] * step + ofs;
            last.col = s->chunk_dims[1] * step + ofs;
        } else {
            size[0] = s->chunk_dims[0];
            size[1] = s->chunk_dims[1] * (1 + step);
            last.row = 0;
            last.col = s->chunk_dims[1] * step + ofs;
        }

        dbgf(1, "new size %" PRIuHSIZE ", %" PRIuHSIZE "\n", size[0], size[1]);
        dbgf(1, "last row %" PRIuHSIZE " col %" PRIuHSIZE "\n", last.row,
            last.col);

        if (s->two_dee) {

            /* Down the right side, intersecting the bottom row. */
            base.col = last.col;
            for (base.row = ofs; base.row <= last.row;
                 base.row += s->chunk_dims[0]) {
                verify_chunk(s, filespace, mat, which, base);
            }

            /* Across the bottom, stopping before the last column to
             * avoid re-writing the bottom-right chunk.
             */
            base.row = last.row;
            for (base.col = ofs; base.col < last.col;
                 base.col += s->chunk_dims[1]) {
                verify_chunk(s, filespace, mat, which, base);
            }
        } else {
            verify_chunk(s, filespace, mat, which, last);
        }
        if (s->asteps != 0 && step % s->asteps == 0)
            verify_dset_attribute(ds, which, step);
    }

    *stepp = last_step;

out:
    if (H5Sclose(filespace) < 0)
        errx(EXIT_FAILURE, "H5Sclose failed");
}

static void
add_dset_attribute(const state_t *s, hid_t ds, hid_t sid, unsigned int which,
    unsigned int step)
{
    hid_t aid;
    char name[sizeof("attr-9999999999")];

    esnprintf(name, sizeof(name), "attr-%u", step);

    dbgf(1, "setting attribute %s on dataset %u to %u\n", name, which, step);

    if ((aid = H5Acreate2(ds, name, s->filetype, sid, H5P_DEFAULT,
            H5P_DEFAULT)) < 0)
        errx(EXIT_FAILURE, "H5Acreate2 failed");

    if (H5Awrite(aid, H5T_NATIVE_UINT, &step) < 0)
        errx(EXIT_FAILURE, "H5Awrite failed");
    if (H5Aclose(aid) < 0)
        errx(EXIT_FAILURE, "H5Aclose failed");
}

static void
write_extensible_dset(state_t *s, unsigned int which, unsigned int step,
    mat_t *mat)
{
    hid_t ds, filespace;
    hsize_t size[RANK];
    base_t base, last;

    dbgf(1, "%s: which %u step %u\n", __func__, which, step);

    assert(which < s->ndatasets);

    ds = s->dataset[which];

    if (s->asteps != 0 && step % s->asteps == 0)
        add_dset_attribute(s, ds, s->one_by_one_sid, which, step);

    if (s->two_dee) {
        size[0] = s->chunk_dims[0] * (1 + step);
        size[1] = s->chunk_dims[1] * (1 + step);
        last.row = s->chunk_dims[0] * step;
        last.col = s->chunk_dims[1] * step;
    } else {
        size[0] = s->chunk_dims[0];
        size[1] = s->chunk_dims[1] * (1 + step);
        last.row = 0;
        last.col = s->chunk_dims[1] * step;
    }

    dbgf(1, "new size %" PRIuHSIZE ", %" PRIuHSIZE "\n", size[0], size[1]);

    // if use_vds, then set_extent for each underlying dataset?

    if (H5Dset_extent(ds, size) < 0)
        errx(EXIT_FAILURE, "H5Dset_extent failed");

    filespace = H5Dget_space(ds);

    if (filespace == badhid)
        errx(EXIT_FAILURE, "H5Dget_space failed");

    if (s->two_dee) {
        base.col = last.col;
        for (base.row = 0; base.row <= last.row; base.row += s->chunk_dims[0]) {
            dbgf(1, "writing chunk %" PRIuHSIZE ", %" PRIuHSIZE "\n",
                base.row, base.col);
            init_and_write_chunk(s, filespace, mat, which, base);
        }

        base.row = last.row;
        for (base.col = 0; base.col < last.col; base.col += s->chunk_dims[1]) {
            dbgf(1, "writing chunk %" PRIuHSIZE ", %" PRIuHSIZE "\n",
                base.row, base.col);
            init_and_write_chunk(s, filespace, mat, which, base);
        }
    } else {
        init_and_write_chunk(s, filespace, mat, which, last);
    }

    if (H5Sclose(filespace) < 0)
            errx(EXIT_FAILURE, "H5Sclose failed");
}

int
main(int argc, char **argv)
{
    mat_t *mat;
    hid_t fapl, fcpl;
    sigset_t oldsigs;
    herr_t ret;
    unsigned step, which;
    bool writer;
    state_t s;
    const char *personality;

    state_init(&s, argc, argv);

    personality = strstr(s.progname, "vfd_swmr_bigset_");

    if (personality != NULL &&
        strcmp(personality, "vfd_swmr_bigset_writer") == 0)
        writer = true;
    else if (personality != NULL &&
             strcmp(personality, "vfd_swmr_bigset_reader") == 0)
        writer = false;
    else {
        errx(EXIT_FAILURE,
             "unknown personality, expected vfd_swmr_bigset_{reader,writer}");
    }

    if ((mat = newmat(s.rows, s.cols)) == NULL)
        err(EXIT_FAILURE, "%s: could not allocate matrix", __func__);

    fapl = vfd_swmr_create_fapl(writer, true, s.use_vfd_swmr);

    if (fapl < 0)
        errx(EXIT_FAILURE, "vfd_swmr_create_fapl");

    if ((fcpl = H5Pcreate(H5P_FILE_CREATE)) < 0)
        errx(EXIT_FAILURE, "H5Pcreate");

    ret = H5Pset_file_space_strategy(fcpl, H5F_FSPACE_STRATEGY_PAGE, false, 1);
    if (ret < 0)
        errx(EXIT_FAILURE, "H5Pset_file_space_strategy");

    if ((s.dapl = H5Pcreate(H5P_DATASET_ACCESS)) < 0)
        errx(EXIT_FAILURE, "%s.%d: H5Pcreate failed", __func__, __LINE__);

    if (H5Pset_chunk_cache(s.dapl, 0, 0,
                        H5D_CHUNK_CACHE_W0_DEFAULT) < 0)
        errx(EXIT_FAILURE, "H5Pset_chunk_cache failed");

    if (writer)
        s.file = H5Fcreate(s.filename, H5F_ACC_TRUNC, fcpl, fapl);
    else
        s.file = H5Fopen(s.filename, H5F_ACC_RDONLY, fapl);

    if (s.file == badhid)
        errx(EXIT_FAILURE, writer ? "H5Fcreate" : "H5Fopen");

    block_signals(&oldsigs);

    if (writer) {
        for (which = 0; which < s.ndatasets; which++)
            create_extensible_dset(&s, which);

        for (step = 0; step < s.nsteps; step++) {
            for (which = 0; which < s.ndatasets; which++) {
                dbgf(2, "step %d which %d\n", step, which);
                write_extensible_dset(&s, which, step, mat);
                if (s.ndatasets <= s.nsteps)
                    nanosleep(&s.update_interval, NULL);
            }
            if (s.ndatasets > s.nsteps)
                nanosleep(&s.update_interval, NULL);
        }
    } else {
        for (which = 0; which < s.ndatasets; which++)
            open_extensible_dset(&s, which);

        for (step = 0; hang_back + step < s.nsteps;) {
            for (which = s.ndatasets; which-- > 0; ) {
                dbgf(2, "step %d which %d\n", step, which);
                verify_extensible_dset(&s, which, mat, &step);
                if (s.ndatasets <= s.nsteps)
                    nanosleep(&s.update_interval, NULL);
            }
            if (s.ndatasets > s.nsteps)
                nanosleep(&s.update_interval, NULL);
        }
    }

    for (which = 0; which < s.ndatasets; which++)
        close_extensible_dset(&s, which);

    if (s.use_vfd_swmr && s.wait_for_signal)
        await_signal(s.file);

    restore_signals(&oldsigs);

    if (H5Pclose(fapl) < 0)
        errx(EXIT_FAILURE, "H5Pclose(fapl)");

    if (H5Pclose(fcpl) < 0)
        errx(EXIT_FAILURE, "H5Pclose(fcpl)");

    state_destroy(&s);

    free(mat);

    return EXIT_SUCCESS;
}