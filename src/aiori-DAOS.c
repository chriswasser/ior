/*
 * -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 */
/*
 * SPECIAL LICENSE RIGHTS-OPEN SOURCE SOFTWARE
 * The Government's rights to use, modify, reproduce, release, perform, display,
 * or disclose this software are subject to the terms of Contract No. B599860,
 * and the terms of the GNU General Public License version 2.
 * Any reproduction of computer software, computer software documentation, or
 * portions thereof marked with this legend must also reproduce the markings.
 */
/*
 * Copyright (c) 2013, 2016 Intel Corporation.
 */
/*
 * This file implements the abstract I/O interface for DAOS.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdint.h>
#include <assert.h>
#include <unistd.h>
#include <strings.h>
#include <sys/types.h>
#include <libgen.h>
#include <stdbool.h>
#include <gurt/common.h>
#include <daos.h>
#include <daos_types.h>
#include <daos_addons.h>

#include "ior.h"
#include "aiori.h"
#include "iordef.h"
#include "list.h"

/************************** O P T I O N S *****************************/
struct daos_options{
        char		*pool;
        char		*svcl;
        char		*group;
	char		*cont;
	int		chunk_size;
	int		destroy;
	char		*oclass;
};

static struct daos_options o = {
        .pool		= NULL,
        .svcl		= NULL,
        .group		= NULL,
	.cont		= NULL,
	.chunk_size	= 1048576,
	.destroy	= 0,
	.oclass		= NULL,
};

static option_help options [] = {
      {0, "daos.pool", "pool uuid", OPTION_REQUIRED_ARGUMENT, 's', &o.pool},
      {0, "daos.svcl", "pool SVCL", OPTION_REQUIRED_ARGUMENT, 's', &o.svcl},
      {0, "daos.group", "server group", OPTION_OPTIONAL_ARGUMENT, 's', &o.group},
      {0, "daos.cont", "container uuid", OPTION_REQUIRED_ARGUMENT, 's', &o.cont},
      {0, "daos.chunk_size", "chunk size", OPTION_OPTIONAL_ARGUMENT, 'd', &o.chunk_size},
      {0, "daos.destroy", "Destroy Container", OPTION_FLAG, 'd', &o.destroy},
      {0, "daos.oclass", "object class", OPTION_OPTIONAL_ARGUMENT, 's', &o.oclass},
      LAST_OPTION
};

/**************************** P R O T O T Y P E S *****************************/

static void DAOS_Init();
static void DAOS_Fini();
static void *DAOS_Create(char *, IOR_param_t *);
static void *DAOS_Open(char *, IOR_param_t *);
static int DAOS_Access(const char *, int, IOR_param_t *);
static IOR_offset_t DAOS_Xfer(int, void *, IOR_size_t *,
                              IOR_offset_t, IOR_param_t *);
static void DAOS_Close(void *, IOR_param_t *);
static void DAOS_Delete(char *, IOR_param_t *);
static char* DAOS_GetVersion();
static void DAOS_Fsync(void *, IOR_param_t *);
static IOR_offset_t DAOS_GetFileSize(IOR_param_t *, MPI_Comm, char *);
static option_help * DAOS_options();

/************************** D E C L A R A T I O N S ***************************/

ior_aiori_t daos_aiori = {
        .name		= "DAOS",
        .create		= DAOS_Create,
        .open		= DAOS_Open,
        .access		= DAOS_Access,
        .xfer		= DAOS_Xfer,
        .close		= DAOS_Close,
        .delete		= DAOS_Delete,
        .get_version	= DAOS_GetVersion,
        .fsync		= DAOS_Fsync,
        .get_file_size	= DAOS_GetFileSize,
        .initialize	= DAOS_Init,
        .finalize	= DAOS_Fini,
	.get_options	= DAOS_options,
};

#define IOR_DAOS_MUR_SEED 0xDEAD10CC

enum handleType {
        POOL_HANDLE,
        CONT_HANDLE,
	ARRAY_HANDLE
};

static daos_handle_t	poh;
static daos_handle_t	coh;
static daos_handle_t	aoh;
static daos_oclass_id_t objectClass = DAOS_OC_LARGE_RW;
static bool		daos_initialized = false;

/***************************** F U N C T I O N S ******************************/

/* For DAOS methods. */
#define DCHECK(rc, format, ...)                                         \
do {                                                                    \
        int _rc = (rc);                                                 \
                                                                        \
        if (_rc < 0) {                                                  \
                fprintf(stdout, "ior ERROR (%s:%d): %d: %d: "           \
                        format"\n", __FILE__, __LINE__, rank, _rc,      \
                        ##__VA_ARGS__);                                 \
                fflush(stdout);                                         \
                MPI_Abort(MPI_COMM_WORLD, -1);                          \
        }                                                               \
} while (0)

#define INFO(level, format, ...)					\
do {                                                                    \
        if (verbose >= level)						\
                printf("[%d] "format"\n", rank, ##__VA_ARGS__);         \
} while (0)

/* For generic errors like invalid command line options. */
#define GERR(format, ...)                                               \
do {                                                                    \
        fprintf(stdout, format"\n", ##__VA_ARGS__);                     \
        MPI_CHECK(MPI_Abort(MPI_COMM_WORLD, -1), "MPI_Abort() error");  \
} while (0)

/* Distribute process 0's pool or container handle to others. */
static void
HandleDistribute(daos_handle_t *handle, enum handleType type)
{
        daos_iov_t global;
        int        rc;

        global.iov_buf = NULL;
        global.iov_buf_len = 0;
        global.iov_len = 0;

        if (rank == 0) {
                /* Get the global handle size. */
                if (type == POOL_HANDLE)
                        rc = daos_pool_local2global(*handle, &global);
                else if (type == CONT_HANDLE)
                        rc = daos_cont_local2global(*handle, &global);
		else
			rc = daos_array_local2global(*handle, &global);
                DCHECK(rc, "Failed to get global handle size");
        }

        MPI_CHECK(MPI_Bcast(&global.iov_buf_len, 1, MPI_UINT64_T, 0,
                            MPI_COMM_WORLD),
                  "Failed to bcast global handle buffer size");

        global.iov_buf = malloc(global.iov_buf_len);
        if (global.iov_buf == NULL)
                ERR("Failed to allocate global handle buffer");

        if (rank == 0) {
                if (type == POOL_HANDLE)
                        rc = daos_pool_local2global(*handle, &global);
                else if (type == CONT_HANDLE) 
                        rc = daos_cont_local2global(*handle, &global);
		else
			rc = daos_array_local2global(*handle, &global);
                DCHECK(rc, "Failed to create global handle");
        }

        MPI_CHECK(MPI_Bcast(global.iov_buf, global.iov_buf_len, MPI_BYTE, 0,
                            MPI_COMM_WORLD),
                  "Failed to bcast global pool handle");

        if (rank != 0) {
                /* A larger-than-actual length works just fine. */
                global.iov_len = global.iov_buf_len;

                if (type == POOL_HANDLE)
                        rc = daos_pool_global2local(global, handle);
                else if (type == CONT_HANDLE)
                        rc = daos_cont_global2local(poh, global, handle);
		else
			rc = daos_array_global2local(coh, global, handle);
                DCHECK(rc, "Failed to get local handle");
        }

        free(global.iov_buf);
}

static void
ObjectClassParse(const char *string)
{
        if (strcasecmp(string, "tiny") == 0)
                objectClass = DAOS_OC_TINY_RW;
        else if (strcasecmp(string, "small") == 0)
                objectClass = DAOS_OC_SMALL_RW;
        else if (strcasecmp(string, "large") == 0)
                objectClass = DAOS_OC_LARGE_RW;
        else if (strcasecmp(string, "echo_tiny") == 0)
                objectClass = DAOS_OC_ECHO_TINY_RW;
        else if (strcasecmp(string, "echo_R2S") == 0)
                objectClass = DAOS_OC_ECHO_R2S_RW;
        else if (strcasecmp(string, "echo_R3S") == 0)
                objectClass = DAOS_OC_ECHO_R3S_RW;
        else if (strcasecmp(string, "echo_R4S") == 0)
                objectClass = DAOS_OC_ECHO_R4S_RW;
        else if (strcasecmp(string, "R2") == 0)
                objectClass = DAOS_OC_R2_RW;
        else if (strcasecmp(string, "R2S") == 0)
                objectClass = DAOS_OC_R2S_RW;
        else if (strcasecmp(string, "R3S") == 0)
                objectClass = DAOS_OC_R3S_RW;
        else if (strcasecmp(string, "R3") == 0)
                objectClass = DAOS_OC_R3_RW;
        else if (strcasecmp(string, "R4") == 0)
                objectClass = DAOS_OC_R4_RW;
        else if (strcasecmp(string, "R4S") == 0)
                objectClass = DAOS_OC_R4S_RW;
        else if (strcasecmp(string, "repl_max") == 0)
                objectClass = DAOS_OC_REPL_MAX_RW;
        else
                GERR("Invalid 'oclass' argument: '%s'", string);
}

static option_help *
DAOS_options()
{
  return options;
}

static void
DAOS_Init()
{
        int rc;

	if (daos_initialized)
		return;

	if (o.pool == NULL || o.svcl == NULL || o.cont == NULL)
		return;
        if (o.oclass)
                ObjectClassParse(o.oclass);

        rc = daos_init();
	if (rc)
		DCHECK(rc, "Failed to initialize daos");

        if (rank == 0) {
                uuid_t			uuid;
		d_rank_list_t		*svcl = NULL;
                d_rank_list_t    	ranks;
		static daos_pool_info_t po_info;
		static daos_cont_info_t co_info;

                INFO(VERBOSE_1, "Connecting to pool %s", o.pool);

                rc = uuid_parse(o.pool, uuid);
                DCHECK(rc, "Failed to parse 'pool': %s", o.pool);

		svcl = daos_rank_list_parse(o.svcl, ":");
		if (svcl == NULL)
			ERR("Failed to allocate svcl");

                rc = daos_pool_connect(uuid, o.group, svcl, DAOS_PC_RW,
				       &poh, &po_info, NULL);
		daos_rank_list_free(svcl);
                DCHECK(rc, "Failed to connect to pool %s", o.pool);

                INFO(VERBOSE_1, "Create/Open Container %s", o.cont);

		uuid_clear(uuid);
		rc = uuid_parse(o.cont, uuid);
		DCHECK(rc, "Failed to parse 'cont': %s", o.cont);

		rc = daos_cont_open(poh, uuid, DAOS_COO_RW, &coh, &co_info,
				    NULL);
		/* If NOEXIST we create it */
		if (rc == -DER_NONEXIST) {
			INFO(VERBOSE_2, "Creating DAOS Container...\n");
			rc = daos_cont_create(poh, uuid, NULL, NULL);
			if (rc == 0)
				rc = daos_cont_open(poh, uuid, DAOS_COO_RW,
						    &coh, &co_info, NULL);
		}
		DCHECK(rc, "Failed to create container");
        }

        HandleDistribute(&poh, POOL_HANDLE);
        HandleDistribute(&coh, CONT_HANDLE);
	aoh.cookie = 0;

	daos_initialized = true;
}

static void
DAOS_Fini()
{
        int rc;

	if (!daos_initialized)
		return;

	rc = daos_cont_close(coh, NULL);
	DCHECK(rc, "Failed to close container\n");

	if (o.destroy) {
		if (rank == 0) {
			uuid_t uuid;

			INFO(VERBOSE_1, "Destroying Container %s", o.cont);
			uuid_parse(o.cont, uuid);
			rc = daos_cont_destroy(poh, o.cont, 1, NULL);
		}

		MPI_Bcast(&rc, 1, MPI_INT, 0, MPI_COMM_WORLD);
		if (rc)
			DCHECK(rc, "Failed to destroy container %s (%d)",
			       o.cont, rc);
	}

	rc = daos_pool_disconnect(poh, NULL);
	DCHECK(rc, "Failed to disconnect from pool %s", o.pool);

        rc = daos_fini();
        DCHECK(rc, "Failed to finalize daos");

	daos_initialized = false;
}

static void
gen_oid(const char *name, daos_obj_id_t *oid)
{
	daos_ofeat_t feat = 0;

	oid->lo = d_hash_murmur64(name, strlen(name), IOR_DAOS_MUR_SEED);
	oid->hi = 0;

	feat = DAOS_OF_DKEY_UINT64 | DAOS_OF_AKEY_HASHED;
	daos_obj_generate_id(oid, feat, objectClass);
}

static void *
DAOS_Create(char *testFileName, IOR_param_t *param)
{
	daos_obj_id_t	oid;
	int		rc;

	/** Convert file name into object ID */
	gen_oid(testFileName, &oid);

	/** Create the array */
	if (param->filePerProc || rank == 0) {
		printf("Chunk size = %zu\n", o.chunk_size);
		rc = daos_array_create(coh, oid, DAOS_TX_NONE, 1, o.chunk_size,
				       &aoh, NULL);
		DCHECK(rc, "Failed to create array object\n");
	}

	/** Distribute the array handle if not FPP */
	if (!param->filePerProc)
		HandleDistribute(&aoh, ARRAY_HANDLE);
}

static int
DAOS_Access(const char *testFileName, int mode, IOR_param_t * param)
{
	daos_obj_id_t	oid;
	int		rc;

	/** Convert file name into object ID */
	gen_oid(testFileName, &oid);

	/** open the array to verify it exists */
	if (param->filePerProc || rank == 0) {
		daos_size_t cell_size, chunk_size;

		rc = daos_array_open(coh, oid, DAOS_TX_NONE, DAOS_OO_RO,
				     &cell_size, &chunk_size, &aoh, NULL);
		if (rc)
			return rc;

		if (cell_size != 1)
			GERR("Invalid DAOS Array object.\n");

		rc = daos_array_close(aoh, NULL);
		aoh.cookie = 0;
	}

	if (!param->filePerProc)
		MPI_Bcast(&rc, 1, MPI_INT, 0, MPI_COMM_WORLD);

	return rc;
}

static void *
DAOS_Open(char *testFileName, IOR_param_t *param)
{
	daos_obj_id_t	oid;

	/** Convert file name into object ID */
	gen_oid(testFileName, &oid);

	/** Open the array */
	if (param->filePerProc || rank == 0) {
		daos_size_t cell_size, chunk_size;
		int rc;

		rc = daos_array_open(coh, oid, DAOS_TX_NONE, DAOS_OO_RW,
				     &cell_size, &chunk_size, &aoh, NULL);
		DCHECK(rc, "Failed to create array object\n");

		if (cell_size != 1)
			GERR("Invalid DAOS Array object.\n");
	}

	/** Distribute the array handle if not FPP */
	if (!param->filePerProc)
		HandleDistribute(&aoh, ARRAY_HANDLE);
}

static IOR_offset_t
DAOS_Xfer(int access, void *file, IOR_size_t *buffer,
	  IOR_offset_t length, IOR_param_t *param)
{
	daos_array_iod_t        iod;
	daos_range_t            rg;
	daos_sg_list_t		sgl;
	daos_iov_t		iov;
	int			rc;

	/** set array location */
	iod.arr_nr = 1;
	rg.rg_len = length;
	rg.rg_idx = param->offset;
	iod.arr_rgs = &rg;

	/** set memory location */
	sgl.sg_nr = 1;
	daos_iov_set(&iov, buffer, length);
	sgl.sg_iovs = &iov;

        if (access == WRITE) {
		rc = daos_array_write(aoh, DAOS_TX_NONE, &iod, &sgl, NULL, NULL);
                DCHECK(rc, "daos_array_write() failed (%d).", rc);
	} else {
		rc = daos_array_read(aoh, DAOS_TX_NONE, &iod, &sgl, NULL, NULL);
                DCHECK(rc, "daos_array_read() failed (%d).", rc);
	}

	return length;
}

static void
DAOS_Close(void *file, IOR_param_t *param)
{
        int rc;

	if (!daos_initialized)
		GERR("DAOS is not initialized!");

	rc = daos_array_close(aoh, NULL);
	DCHECK(rc, "daos_array_close() failed (%d).", rc);

	aoh.cookie = 0;
}

static void
DAOS_Delete(char *testFileName, IOR_param_t *param)
{
	daos_obj_id_t	oid;
        int		rc;

	if (!daos_initialized)
		GERR("DAOS is not initialized!");

	/** Convert file name into object ID */
	gen_oid(testFileName, &oid);

	/** open the array to verify it exists */
	if (param->filePerProc || rank == 0) {
		daos_size_t cell_size, chunk_size;

		rc = daos_array_open(coh, oid, DAOS_TX_NONE, DAOS_OO_RW,
				     &cell_size, &chunk_size, &aoh, NULL);
		DCHECK(rc, "daos_array_open() failed (%d).", rc);

		if (cell_size != 1)
			GERR("Invalid DAOS Array object.\n");

		rc = daos_array_destroy(aoh, DAOS_TX_NONE, NULL);
		DCHECK(rc, "daos_array_destroy() failed (%d).", rc);

		rc = daos_array_close(aoh, NULL);
		DCHECK(rc, "daos_array_close() failed (%d).", rc);
		aoh.cookie = 0;
	}

	if (!param->filePerProc)
		MPI_Bcast(&rc, 1, MPI_INT, 0, MPI_COMM_WORLD);
}

static char *
DAOS_GetVersion()
{
	static char ver[1024] = {};

	sprintf(ver, "%s", "DAOS");
	return ver;
}

static void
DAOS_Fsync(void *file, IOR_param_t *param)
{
	return;
}

static IOR_offset_t
DAOS_GetFileSize(IOR_param_t *param, MPI_Comm testComm, char *testFileName)
{
	daos_obj_id_t	oid;
	daos_size_t	size;
        int		rc;

	if (!daos_initialized)
		GERR("DAOS is not initialized!");

	/** Convert file name into object ID */
	gen_oid(testFileName, &oid);

	/** open the array to verify it exists */
	if (param->filePerProc || rank == 0) {
		daos_size_t cell_size, chunk_size;

		rc = daos_array_open(coh, oid, DAOS_TX_NONE, DAOS_OO_RO,
				     &cell_size, &chunk_size, &aoh, NULL);
		DCHECK(rc, "daos_array_open() failed (%d).", rc);

		if (cell_size != 1)
			GERR("Invalid DAOS Array object.\n");

		rc = daos_array_get_size(aoh, DAOS_TX_NONE, &size, NULL);
		DCHECK(rc, "daos_array_get_size() failed (%d).", rc);

		rc = daos_array_close(aoh, NULL);
		DCHECK(rc, "daos_array_close() failed (%d).", rc);
		aoh.cookie = 0;
	}

	if (!param->filePerProc)
		MPI_Bcast(&size, 1, MPI_LONG, 0, MPI_COMM_WORLD);

	return size;
}
