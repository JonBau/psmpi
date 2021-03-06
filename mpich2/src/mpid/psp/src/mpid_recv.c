/*
 * ParaStation
 *
 * Copyright (C) 2006-2010 ParTec Cluster Competence Center GmbH, Munich
 *
 * This file may be distributed under the terms of the Q Public License
 * as defined in the file LICENSE.QPL included in the packaging of this
 * file.
 *
 * Author:	Jens Hauke <hauke@par-tec.com>
 */

#include "mpidimpl.h"

#define FCNAME "MPID_Recv"
#define FUNCNAME MPID_Recv
int MPID_Recv(void * buf, MPI_Aint count, MPI_Datatype datatype, int rank, int tag,
	      MPID_Comm * comm, int context_offset, MPI_Status * status, MPID_Request ** request)
{
	int mpi_errno;
/*	printf("#%d ps--- %s() called\n", MPIDI_Process.my_pg_rank, __func__); */

	mpi_errno = MPID_Irecv(buf, count, datatype, rank, tag, comm, context_offset, request);
	if (mpi_errno == MPI_SUCCESS) {
		mpi_errno = MPID_PSP_Wait(*request);
	}

	return mpi_errno;
}
#undef FUNCNAME
#undef FCNAME
