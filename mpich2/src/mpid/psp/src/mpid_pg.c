/*
 * ParaStation
 *
 * Copyright (C) 2006-2014 ParTec Cluster Competence Center GmbH, Munich
 *
 * This file may be distributed under the terms of the Q Public License
 * as defined in the file LICENSE.QPL included in the packaging of this
 * file.
 *
 * Author:	Carsten Clauss <clauss@par-tec.com>
 */

#include "mpidimpl.h"
#include "mpiinfo.h"

int MPID_GPID_Get( MPID_Comm *comm_ptr, int rank, MPID_Gpid *gpid )
{
	MPIDI_VC_t *vc;

	vc = comm_ptr->vcrt->vcr[rank];

	/* Get the process group id as an int */
	gpid->gpid[0] = vc->pg->id_num;
	gpid->gpid[1] = vc->pg_rank;

	return 0;
}

/* see intercomm_create.c: */
int MPID_GPID_GetAllInComm(MPID_Comm *comm_ptr, int local_size,
			   MPID_Gpid *local_gpids, int *singlePG)
{
	int i;
	MPID_Gpid *gpid = local_gpids;
	int lastPGID = -1;

	if(singlePG) *singlePG = 1;
	for (i=0; i<comm_ptr->local_size; i++) {
		MPID_GPID_Get(comm_ptr, i, gpid);

		if (lastPGID != gpid->gpid[0]) {
			if (i == 0) {
				lastPGID = gpid->gpid[0];
			} else {
				if(singlePG) *singlePG = 0;
			}
		}
		gpid++;
	}
	return 0;
}

int MPID_GPID_ToLpidArray(int size, MPID_Gpid gpid[], int lpid[])
{
	int i, mpi_errno = MPI_SUCCESS;
	int pgid;
	MPIDI_PG_t *pg;

	for (i=0; i<size; i++) {

		pg = MPIDI_Process.my_pg;

		do {

			if(!pg) {
				/* Unknown process group! This can happen if two (or more) PG have been spawned in the meanwhile...
				   The best we can do here is to create a new one:
				   (size unknown, but at least as big as the current PG rank plus one)
				*/
				MPIDI_PG_t *new_pg;

				MPIDI_PG_Create(gpid->gpid[1]+1, gpid->gpid[0], &new_pg);

				assert(new_pg->lpids[gpid->gpid[1]] == -1);
				if (!MPIDI_Process.next_lpid) MPIDI_Process.next_lpid = MPIR_Process.comm_world->local_size;
				lpid[i] = MPIDI_Process.next_lpid++;
				new_pg->lpids[gpid->gpid[1]] = lpid[i];

				break;

			} else {

				pgid = pg->id_num;

				if (pgid == gpid->gpid[0]) {
					/* found the process group.  gpid->gpid[1] is the rank in this process group */

					/* Sanity check on size */
					if(gpid->gpid[1] >= pg->size) {
						/* This can happen if a new PG was created (see above) but the initially chosen size was too small
						   (which is quite likely to happen). Now, we have to re-size the PG: (ugly but effective...)
						*/
						int k = pg->size;

						pg->size = gpid->gpid[1]+1;
						pg->vcr = MPIU_Realloc(pg->vcr, sizeof(MPIDI_VC_t*)*pg->size);
						pg->lpids = MPIU_Realloc(pg->lpids, sizeof(int)*pg->size);
						pg->cons = MPIU_Realloc(pg->cons, sizeof(pscom_connection_t*)*pg->size);

						for(; k<pg->size; k++) {
							pg->vcr[k] = NULL;
							pg->lpids[k] = -1;
							pg->cons[k] = NULL;
						}
					}

					if(!pg->vcr[gpid->gpid[1]]) {
						assert(pg->lpids[gpid->gpid[1]] == -1);
						/* VCR not yet initialized (connection establishment still needed)
						   Assign next free LPID (MPIDI_Process.next_lpid):
						*/
						if (!MPIDI_Process.next_lpid) MPIDI_Process.next_lpid = MPIR_Process.comm_world->local_size;
						lpid[i] = MPIDI_Process.next_lpid++;
						/*printf("(%d) LPID NOT found! Assigned next lipd: %d\n", getpid(), lpid[i]);*/
						pg->lpids[gpid->gpid[1]] = lpid[i];
						break;
					}

					lpid[i] = pg->vcr[gpid->gpid[1]]->lpid;

					break;
				}
				pg = pg->next;
			}
		} while (1);

		gpid++;
	}

	return mpi_errno;
}

int MPID_Create_intercomm_from_lpids(MPID_Comm *newcomm_ptr, int size, const int lpids[] )
{
	int mpi_errno = MPI_SUCCESS;
	MPID_Comm *commworld_ptr;
	MPIDI_VCRT_t *vcrt;
	int i;

	commworld_ptr = MPIR_Process.comm_world;
	/* Setup the communicator's vc table: remote group */
	vcrt = MPIDI_VCRT_Create(size);
	assert(vcrt);
	MPID_PSP_comm_set_vcrt(newcomm_ptr, vcrt);

	for (i=0; i<size; i++) {
		MPIDI_VC_t *vcr = NULL;

		/* For rank i in the new communicator, find the corresponding
		   virtual connection.  For lpids less than the size of comm_world,
		   we can just take the corresponding entry from comm_world.
		   Otherwise, we need to search through the process groups.
		*/
		/* printf( "[%d] Remote rank %d has lpid %d\n",
		   MPIR_Process.comm_world->rank, i, lpids[i] ); */
		if ((lpids[i] >=0) && (lpids[i] < commworld_ptr->remote_size)) {
			vcr = commworld_ptr->vcr[lpids[i]];
			assert(vcr);
		}
		else {
			/* We must find the corresponding vcr for a given lpid */
			/* For now, this means iterating through the process groups */
			MPIDI_PG_t *pg;
			int j;

			pg = MPIDI_Process.my_pg->next; /* (skip comm_world) */

			do {
				assert(pg);

				for (j=0; j<pg->size; j++) {

					if(!pg->vcr[j]) continue;

					if (pg->vcr[j]->lpid == lpids[i]) {
						/* Found vc for current lpid in another pg! */
						vcr = pg->vcr[j];
						break;
					}
				}
				pg = pg->next;
			} while (!vcr);
		}

		/* Note that his will increment the ref count for the associate PG if necessary.  */
		newcomm_ptr->vcr[i] = MPIDI_VC_Dup(vcr);
	}

fn_exit:
	return mpi_errno;
fn_fail:
	goto fn_exit;
}


/* The following is a temporary hook to ensure that all processes in
   a communicator have a set of process groups.

   All arguments are input (all processes in comm must have gpids)

   First: all processes check to see if they have information on all
   of the process groups mentioned by id in the array of gpids.

   The local result is LANDed with Allreduce.
   If any process is missing process group information, then the
   root process broadcasts the process group information as a string;
   each process then uses this information to update to local process group
   information and then connects to the still missing remote partners.
*/
int MPID_PG_ForwardPGInfo( MPID_Comm *peer_comm_ptr, MPID_Comm *comm_ptr,
			   int nPGids, const MPID_Gpid gpids[], int root, int remote_leader, int cts_tag,
			   pscom_connection_t *peer_con, char *all_ports, pscom_socket_t *pscom_socket )
{
	MPIR_Errflag_t errflag = FALSE;
	int mpi_errno = MPI_SUCCESS;
	pscom_err_t rc;

	int i, j;
	MPIDI_PG_t *pg;
	int id_num;

	const MPID_Gpid *gpid_ptr;
	int all_found_local = 1;
	int all_found_remote;
	int pg_count_root = 0;
	int pg_count_local = 0;
	int pg_count_remote = 0;

	pg = MPIDI_Process.my_pg;
	do {
		assert(pg);
		pg_count_local++;
		pg = pg->next;

	} while (pg);

	gpid_ptr = gpids;
	for (i=0; i<nPGids; i++) {
		pg = MPIDI_Process.my_pg;
		do {
			if(!pg){
				/* We don't know this pgid... */
				all_found_local = 0;
				break;
			}
			id_num = pg->id_num;
			if(id_num == gpid_ptr->gpid[0]) {
				/* Found PG, but is the respective pg_rank also there? */
				for(j=0; j<pg->size; j++) {
					if( pg->vcr[j] && (pg->vcr[j]->pg_rank == gpid_ptr->gpid[1]) ) {
						/* Found pg_rank! */
						break;
					}
				}
				if(j==pg->size) {
					/* We don't know this pg_rank of the known pgid... */
					all_found_local = 0;
					break;
				}
			}
			pg = pg->next;
		} while (id_num != gpid_ptr->gpid[0]);
		gpid_ptr++;
	}

	/* See if everyone in local comm is happy: */
	mpi_errno = MPIR_Allreduce_impl( MPI_IN_PLACE, &all_found_local, 1, MPI_INT, MPI_LAND, comm_ptr, &errflag );
	assert(mpi_errno == MPI_SUCCESS);

	/* See if remote procs are happy, too: */
	if(comm_ptr->rank == root) {
		if(peer_comm_ptr) {
			mpi_errno = MPIC_Sendrecv(&all_found_local,  1, MPI_INT,
						  remote_leader, cts_tag,
						  &all_found_remote, 1, MPI_INT,
						  remote_leader, cts_tag,
						  peer_comm_ptr, MPI_STATUS_IGNORE, &errflag);
			assert(mpi_errno == MPI_SUCCESS);
		} else {
			assert(peer_con);
			pscom_send(peer_con, NULL, 0, &all_found_local, sizeof(int));
			rc = pscom_recv_from(peer_con, NULL, 0, &all_found_remote, sizeof(int));
			assert(rc == PSCOM_SUCCESS);
		}
	}

	/* Check if we can stop this here because all procs involved are happy: */
	mpi_errno = MPIR_Bcast_impl(&all_found_remote, 1, MPI_INT, root, comm_ptr, &errflag);
	assert(mpi_errno == MPI_SUCCESS);

	if(all_found_local && all_found_remote) {
		/* Oh Happy Day! :-) We can leave this here without further ado!
		   (Quite likely we are dealing here with a non-spawn case...)
		*/
		return MPI_SUCCESS;
	}

	if(comm_ptr->rank == root) {

		/* Initially, make sure that all remote PGs are known at root! */

		int* local_pg_ids;
		int* local_pg_sizes;
		int* remote_pg_ids;
		int* remote_pg_sizes;
		int new_pg_count = 0;

		if(peer_comm_ptr) {
			mpi_errno = MPIC_Sendrecv(&pg_count_local, 1, MPI_INT,
						  remote_leader, cts_tag,
						  &pg_count_remote, 1, MPI_INT,
						  remote_leader, cts_tag,
						  peer_comm_ptr, MPI_STATUS_IGNORE, &errflag);
			assert(mpi_errno == MPI_SUCCESS);

		} else {
			assert(peer_con);
			pscom_send(peer_con, NULL, 0, &pg_count_local, sizeof(int));
			rc = pscom_recv_from(peer_con, NULL, 0, &pg_count_remote, sizeof(int));
			assert(rc == PSCOM_SUCCESS);
		}

		local_pg_ids   = MPIU_Malloc(pg_count_local * sizeof(int));
		local_pg_sizes = MPIU_Malloc(pg_count_local * sizeof(int));

		remote_pg_ids   = MPIU_Malloc(pg_count_remote * sizeof(int));
		remote_pg_sizes = MPIU_Malloc(pg_count_remote * sizeof(int));

		pg = MPIDI_Process.my_pg;
		for(i=0; i<pg_count_local; i++) {
			local_pg_ids[i] = pg->id_num;
			local_pg_sizes[i] = pg->size;
			pg = pg->next;
		}

		if(peer_comm_ptr) {
			mpi_errno = MPIC_Sendrecv(local_pg_ids, pg_count_local, MPI_INT,
						  remote_leader, cts_tag,
						  remote_pg_ids, pg_count_remote, MPI_INT,
						  remote_leader, cts_tag,
						  peer_comm_ptr, MPI_STATUS_IGNORE, &errflag);
			assert(mpi_errno == MPI_SUCCESS);

			mpi_errno = MPIC_Sendrecv(local_pg_sizes, pg_count_local, MPI_INT,
						  remote_leader, cts_tag,
						  remote_pg_sizes, pg_count_remote, MPI_INT,
						  remote_leader, cts_tag,
						  peer_comm_ptr, MPI_STATUS_IGNORE, &errflag);
			assert(mpi_errno == MPI_SUCCESS);
		} else {
			assert(peer_con);
			pscom_send(peer_con, NULL, 0, local_pg_ids, pg_count_local * sizeof(int));
			rc = pscom_recv_from(peer_con, NULL, 0, remote_pg_ids, pg_count_remote * sizeof(int));
			assert(rc == PSCOM_SUCCESS);

			pscom_send(peer_con, NULL, 0, local_pg_sizes, pg_count_local * sizeof(int));
			rc = pscom_recv_from(peer_con, NULL, 0, remote_pg_sizes, pg_count_remote * sizeof(int));
			assert(rc == PSCOM_SUCCESS);
		}

		for(i=0; i<pg_count_remote; i++) {

			int found = 0;
			int needed = 0;

			pg = MPIDI_Process.my_pg;
			for(j=0; j<pg_count_local; j++) {
				assert(pg);
				if(remote_pg_ids[i] == pg->id_num) {
					found = 1;
					break;
				}
				pg = pg->next;
			}

			if(!found) {
				/* Unknown Process Group at root: Check if it is actually needed! */
				gpid_ptr = gpids;
				for (j=0; j<nPGids; j++) {
					if(gpid_ptr->gpid[0] == remote_pg_ids[i]) {
						needed = 1;
						break;
					}
					gpid_ptr++;
				}

				if(needed) {
					MPIDI_PG_t * new_pg_ptr;
					MPIDI_PG_Create(remote_pg_sizes[i], remote_pg_ids[i], &new_pg_ptr);
					new_pg_count++;
				}
			}
		}

		MPIU_Free(local_pg_ids);
		MPIU_Free(local_pg_sizes);
		MPIU_Free(remote_pg_ids);
		MPIU_Free(remote_pg_sizes);

		pg_count_root = pg_count_local + new_pg_count;
		pg = MPIDI_Process.my_pg;
	}

	mpi_errno = MPIR_Bcast_impl(&pg_count_root, 1, MPI_INT, root, comm_ptr, &errflag);
	assert(mpi_errno == MPI_SUCCESS);

	for(i=0; i<pg_count_root; i++) {

		int pg_size;
		int pg_id_num;

		if(comm_ptr->rank == root) {
			assert(pg);
			pg_id_num = pg->id_num;
			pg_size   = pg->size;
		}

		mpi_errno = MPIR_Bcast_impl(&pg_size, 1, MPI_INT, root, comm_ptr, &errflag);
		assert(mpi_errno == MPI_SUCCESS);

		mpi_errno = MPIR_Bcast_impl(&pg_id_num, 1, MPI_INT, root, comm_ptr, &errflag);
		assert(mpi_errno == MPI_SUCCESS);


		if(comm_ptr->rank != root) {

			int found = 0;
			int needed = 0;

			pg = MPIDI_Process.my_pg;
			for(j=0; j<pg_count_local; j++) {
				assert(pg);

				if(pg_id_num == pg->id_num) {
					found = 1;
					break;
				}
				pg = pg->next;
			}

			if(!found) {
				/* Unknown Process Group: Check if it is actually needed! */
				gpid_ptr = gpids;
				for (j=0; j<nPGids; j++) {
					if(gpid_ptr->gpid[0] == pg_id_num) {
						needed = 1;
						break;
					}
					gpid_ptr++;
				}

				if(needed) {
					/* New Process Group: */
					MPIDI_PG_t * new_pg_ptr;
					MPIDI_PG_Create(pg_size, pg_id_num, &new_pg_ptr);
				}
			}
		}

		if(comm_ptr->rank == root) {
			pg = pg->next;
		}
	}

	/* Now it's time to establish the still needed connection: */
	{
		int local_size = 0;
		int remote_size = 0;

		pscom_port_str_t *all_ports_local = NULL;
		pscom_port_str_t *all_ports_remote = NULL;
		pscom_socket_t *comm_socket;

		MPID_Gpid my_gpid;
		MPID_Gpid *local_gpids_by_comm;
		MPID_Gpid *remote_gpids_by_comm;

		/* If we get here via MPIR_Intercomm_create_impl(), we have to open a new socket.
		   If not, a socket should already be opened in MPID_Comm_connect()/accept()... */
		if(pscom_socket) {
			comm_socket = pscom_socket;
			all_ports_local = (pscom_port_str_t*)all_ports;
		} else {
			MPID_Comm intercomm_dummy;
			assert(!all_ports);
			/* We just want to get the socket, but open_all_ports() expects an (inter)comm.
			   So we fetch it via an intercomm_dummy: */
			all_ports_local = MPID_PSP_open_all_ports(root, comm_ptr, &intercomm_dummy);
			comm_socket = intercomm_dummy.pscom_socket;
		}

		local_size = comm_ptr->local_size;

		if(comm_ptr->rank == root) {
			if(peer_comm_ptr) {
				mpi_errno = MPIC_Sendrecv(&local_size,  1, MPI_INT,
							  remote_leader, cts_tag,
							  &remote_size, 1, MPI_INT,
							  remote_leader, cts_tag,
							  peer_comm_ptr, MPI_STATUS_IGNORE, &errflag);
				assert(mpi_errno == MPI_SUCCESS);

				all_ports_remote = MPIU_Malloc(remote_size * sizeof(pscom_port_str_t));
				memset(all_ports_remote, 0, remote_size * sizeof(pscom_port_str_t));

				mpi_errno = MPIC_Sendrecv(all_ports_local, local_size * sizeof(pscom_port_str_t), MPI_CHAR,
							  remote_leader, cts_tag,
							  all_ports_remote, remote_size * sizeof(pscom_port_str_t), MPI_CHAR,
							  remote_leader, cts_tag,
							  peer_comm_ptr, MPI_STATUS_IGNORE, &errflag);
				assert(mpi_errno == MPI_SUCCESS);
			} else {
				assert(peer_con);

				pscom_send(peer_con, NULL, 0, &local_size, sizeof(int));
				rc = pscom_recv_from(peer_con, NULL, 0, &remote_size, sizeof(int));
				assert(rc == PSCOM_SUCCESS);

				all_ports_remote = MPIU_Malloc(remote_size * sizeof(pscom_port_str_t));
				memset(all_ports_remote, 0, remote_size * sizeof(pscom_port_str_t));

				pscom_send(peer_con, NULL, 0, all_ports_local, local_size * sizeof(pscom_port_str_t));
				rc = pscom_recv_from(peer_con, NULL, 0, all_ports_remote, remote_size * sizeof(pscom_port_str_t));
				assert(rc == PSCOM_SUCCESS);
			}
		}

		mpi_errno = MPIR_Bcast_impl(&remote_size, 1, MPI_INT, root, comm_ptr, &errflag);
		assert(mpi_errno == MPI_SUCCESS);

		if(comm_ptr->rank != root) {
			all_ports_remote = MPIU_Malloc(remote_size * sizeof(pscom_port_str_t));
			memset(all_ports_remote, 0, remote_size * sizeof(pscom_port_str_t));
		}

		mpi_errno = MPIR_Bcast_impl(all_ports_remote, remote_size * sizeof(pscom_port_str_t), MPI_CHAR, root, comm_ptr, &errflag);
		assert(mpi_errno == MPI_SUCCESS);

		MPID_GPID_Get(comm_ptr, comm_ptr->rank, &my_gpid);

		local_gpids_by_comm = (MPID_Gpid*)MPIU_Malloc(local_size * sizeof(MPID_Gpid));
		remote_gpids_by_comm = (MPID_Gpid*)MPIU_Malloc(remote_size * sizeof(MPID_Gpid));

		mpi_errno = MPIR_Gather_intra(&my_gpid, sizeof(MPID_Gpid), MPI_CHAR,
					      local_gpids_by_comm, sizeof(MPID_Gpid), MPI_CHAR,
					      root, comm_ptr, &errflag);
		assert(mpi_errno == MPI_SUCCESS);

		if(comm_ptr->rank == root) {
			if(peer_comm_ptr) {
				mpi_errno = MPIC_Sendrecv(local_gpids_by_comm, sizeof(MPID_Gpid) * local_size, MPI_CHAR,
							  remote_leader, cts_tag,
							  remote_gpids_by_comm, sizeof(MPID_Gpid) * remote_size, MPI_CHAR,
							  remote_leader, cts_tag,
							  peer_comm_ptr, MPI_STATUS_IGNORE, &errflag);
				assert(mpi_errno == MPI_SUCCESS);
			} else {
				assert(peer_con);
				pscom_send(peer_con, NULL, 0, local_gpids_by_comm, sizeof(MPID_Gpid) * local_size);
				rc = pscom_recv_from(peer_con, NULL, 0, remote_gpids_by_comm, sizeof(MPID_Gpid) * remote_size);
				assert(rc == PSCOM_SUCCESS);
			}
		}

		mpi_errno = MPIR_Bcast_impl(remote_gpids_by_comm, sizeof(MPID_Gpid) * remote_size, MPI_CHAR, root, comm_ptr, &errflag);
		assert(mpi_errno == MPI_SUCCESS);

		/* FIRST RUN: call pscom_connect_socket_str() to establish all needed connections */
		gpid_ptr = gpids;
		for (i=0; i<nPGids; i++) {
			pg = MPIDI_Process.my_pg;
			do {
				id_num = pg->id_num;

				for (j=0; j<pg->size; j++) {

					if((gpid_ptr->gpid[0] == id_num) && (gpid_ptr->gpid[1] == j)) {

						if(pg->vcr[j]) {
							assert(j==pg->vcr[j]->pg_rank);
						} else {
							if(!pg->cons[j]) {
								int pos;
								MPID_Gpid *remote_gpid_ptr;
								pscom_connection_t *con = pscom_open_connection(comm_socket);

								remote_gpid_ptr = remote_gpids_by_comm;
								for(pos=0; pos<remote_size; pos++) {

									if((remote_gpid_ptr->gpid[0] == gpid_ptr->gpid[0]) &&
									   (remote_gpid_ptr->gpid[1] == gpid_ptr->gpid[1])) {
										break;
									}

									remote_gpid_ptr++;
								}
								assert(pos<remote_size);

								rc = pscom_connect_socket_str(con, all_ports_remote[pos]);
								assert(rc == PSCOM_SUCCESS);

								pg->cons[j] = con;
							} else {
								assert(pg->lpids[j] > -1);
							}
						}
					}
				}

				pg = pg->next;

			} while (pg);

			gpid_ptr++;
		}


		/* Workaround for timing of pscom ondemand connections. Be sure both sides have called
		   pscom_connect_socket_str() before using the connections: */
		MPIR_Barrier_impl(comm_ptr, &errflag);
		if(comm_ptr->rank == root) {
			if(peer_comm_ptr) {
				mpi_errno = MPIC_Sendrecv(NULL, 0, MPI_CHAR, remote_leader, cts_tag, NULL, 0, MPI_CHAR,
							  remote_leader, cts_tag,  peer_comm_ptr, MPI_STATUS_IGNORE, &errflag);
				assert(mpi_errno == MPI_SUCCESS);
			} else {
				int dummy = -1;
				assert(peer_con);
				pscom_send(peer_con, NULL, 0, &dummy, sizeof(int));
				rc = pscom_recv_from(peer_con, NULL, 0, &dummy, sizeof(int));
				assert(rc == PSCOM_SUCCESS);
			}
		}
		MPIR_Barrier_impl(comm_ptr, &errflag);

		/* SECOND RUN: store, check and warm-up all new connections: */
		gpid_ptr = gpids;
		for (i=0; i<nPGids; i++) {
			pg = MPIDI_Process.my_pg;
			do {
				id_num = pg->id_num;

				for (j=0; j<pg->size; j++) {

					if((gpid_ptr->gpid[0] == id_num) && (gpid_ptr->gpid[1] == j)) {

						if(pg->vcr[j]) {
							assert(j==pg->vcr[j]->pg_rank);
						} else {
							int pos;
							MPID_Gpid *remote_gpid_ptr;
							pscom_connection_t *con = pg->cons[j];

							remote_gpid_ptr = remote_gpids_by_comm;
							for(pos=0; pos<remote_size; pos++) {

								if((remote_gpid_ptr->gpid[0] == gpid_ptr->gpid[0]) &&
								   (remote_gpid_ptr->gpid[1] == gpid_ptr->gpid[1])) {
									break;
								}

								remote_gpid_ptr++;
							}
							assert(pos<remote_size);

							assert(con);

							if(pg->lpids[j] > 0) {
								pg->vcr[j] = MPIDI_VC_Create(pg, j, con, pg->lpids[j]);
							} else {
								if (!MPIDI_Process.next_lpid) MPIDI_Process.next_lpid = MPIR_Process.comm_world->local_size;

								/* Using the next so far unused lpid > np.*/
								pg->vcr[j] = MPIDI_VC_Create(pg, j, con, MPIDI_Process.next_lpid++);
							}

							/* Sanity check and connection warm-up: */
							if(!MPIDI_Process.env.enable_ondemand_spawn)
							{
								int remote_pg_id;
								int remote_pg_rank;

								/*
								printf("(%d) [%d|%d] --> [%d|%d] %s\n", getpid(), MPIDI_Process.my_pg->id_num, MPIR_Process.comm_world->rank,
									  gpid_ptr[0], gpid_ptr[1], all_ports_remote[pos]);
								*/

								if(MPIDI_Process.my_pg->id_num < gpid_ptr->gpid[0]) {
									pscom_send(con, NULL, 0, &MPIDI_Process.my_pg->id_num, sizeof(int));
									rc = pscom_recv_from(con, NULL, 0, &remote_pg_id, sizeof(int));
									assert(rc == PSCOM_SUCCESS);
									assert(remote_pg_id == gpid_ptr->gpid[0]);

									pscom_send(con, NULL, 0, &MPIR_Process.comm_world->rank, sizeof(int));
									rc = pscom_recv_from(con, NULL, 0, &remote_pg_rank, sizeof(int));
									assert(rc == PSCOM_SUCCESS);
									assert(remote_pg_rank == gpid_ptr->gpid[1]);
								} else {
									rc = pscom_recv_from(con, NULL, 0, &remote_pg_id, sizeof(int));
									assert(rc == PSCOM_SUCCESS);
									assert(remote_pg_id == gpid_ptr->gpid[0]);
									pscom_send(con, NULL, 0, &MPIDI_Process.my_pg->id_num, sizeof(int));

									rc = pscom_recv_from(con, NULL, 0, &remote_pg_rank, sizeof(int));
									assert(rc == PSCOM_SUCCESS);
									assert(remote_pg_rank == gpid_ptr->gpid[1]);
									pscom_send(con, NULL, 0, &MPIR_Process.comm_world->rank, sizeof(int));
								}
							}
						}
					}
				}

				pg = pg->next;

			} while (pg);

			gpid_ptr++;
		}

		MPIU_Free(local_gpids_by_comm);
		MPIU_Free(remote_gpids_by_comm);
		MPIU_Free(all_ports_remote);
		if(!pscom_socket) {
			MPIU_Free(all_ports_local);
		}

		pscom_stop_listen(comm_socket);
	}

	return MPI_SUCCESS;
}


#undef FUNCNAME
#define FUNCNAME MPIDI_PG_Create
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
int MPIDI_PG_Create(int pg_size, int pg_id_num, MPIDI_PG_t ** pg_ptr)
{
	MPIDI_PG_t * pg = NULL, *pgnext;
	int i;
	int mpi_errno = MPI_SUCCESS;
	MPIU_CHKPMEM_DECL(4);
	MPIDI_STATE_DECL(MPID_STATE_MPIDI_PG_CREATE);

	MPIDI_FUNC_ENTER(MPID_STATE_MPIDI_PG_CREATE);

	MPIU_CHKPMEM_MALLOC(pg, MPIDI_PG_t* ,sizeof(MPIDI_PG_t), mpi_errno, "pg");
	MPIU_CHKPMEM_MALLOC(pg->vcr, MPIDI_VC_t**, sizeof(MPIDI_VC_t)*pg_size, mpi_errno,"pg->vcr");
	MPIU_CHKPMEM_MALLOC(pg->lpids, int*, sizeof(int)*pg_size, mpi_errno,"pg->lpids");
	MPIU_CHKPMEM_MALLOC(pg->cons, pscom_connection_t **, sizeof(pscom_connection_t*)*pg_size, mpi_errno,"pg->cons");

	pg->size = pg_size;
	pg->id_num = pg_id_num;
	pg->refcnt = 0;

	for(i=0; i<pg_size; i++) {
		pg->vcr[i] = NULL;
		pg->lpids[i] = -1;
		pg->cons[i] = NULL;
	}

	/* Add pg's at the tail so that comm world is always the first pg */
	pg->next = NULL;

	if (!MPIDI_Process.my_pg)
	{
		/* The first process group is always the world group */
		MPIDI_Process.my_pg = pg;
	}
	else
	{
		pgnext = MPIDI_Process.my_pg;
		while (pgnext->next)
		{
			pgnext = pgnext->next;
		}
		pgnext->next = pg;
	}
	*pg_ptr = pg;

fn_exit:
	MPIDI_FUNC_EXIT(MPID_STATE_MPIDI_PG_CREATE);
	return mpi_errno;

fn_fail:
	MPIU_CHKPMEM_REAP();
	goto fn_exit;
}

#undef FUNCNAME
#define FUNCNAME MPIDI_PG_Destroy
#undef FCNAME
#define FCNAME MPIDI_QUOTE(FUNCNAME)
MPIDI_PG_t* MPIDI_PG_Destroy(MPIDI_PG_t * pg_ptr)
{
	int j;
	MPIDI_PG_t * pg_next = pg_ptr->next;

	/* Check if this is the PG of the local COMM_WORLD.
	   If not, ensure that the list of PGs does not get broken: */
	if(pg_ptr != MPIDI_Process.my_pg) {
		MPIDI_PG_t * pg_run = MPIDI_Process.my_pg;
		assert(pg_run);
		while(pg_run->next != pg_ptr) {
			pg_run = pg_run->next;
			assert(pg_run);
		}
		pg_run->next = pg_next;
	}


	for (j=0; j<pg_ptr->size; j++) {

		assert( (pg_ptr->refcnt>0) || ((pg_ptr->refcnt==0)&&(!pg_ptr->vcr[j])) );

		if (pg_ptr->vcr[j]) {
			/* If MPIDI_PG_Destroy() is called with still existing connections,
			   then this PG has not been disconnected before. Hence, this is most
			   likely the common case, where an MPI_Finalize() is tearing down the
			   current session. Therefore, we just close the still open connections
			   and free the related VCR without any decreasing of reference counters:
			*/
			pscom_close_connection(pg_ptr->vcr[j]->con);
			MPIU_Free(pg_ptr->vcr[j]);

		} else  {

			if(pg_ptr->cons[j]) {
				/* If we come here, this rank has already been disconected in an
				   MPI sense but due to the 'lazy disconnect' feature, the old
				   pscom connection is still open. Hence, close it right here:
				*/
				pscom_close_connection(pg_ptr->cons[j]);
			}
		}
	}

	MPIU_Free(pg_ptr->cons);
	MPIU_Free(pg_ptr->lpids);
	MPIU_Free(pg_ptr->vcr);
	MPIU_Free(pg_ptr);

	return pg_next;
}
#undef FUNCNAME
#undef FCNAME

/* Taken from MPIDI_PG_IdToNum() of CH3: */
void MPIDI_PG_Convert_id(char *pg_id_name, int *pg_id_num)
{
	const char *p = (const char *)pg_id_name;
	int pgid = 0;

	while (*p) {
		pgid += *p++;
		pgid += (pgid << 10);
		pgid ^= (pgid >> 6);
	}
	pgid += (pgid << 3);
	pgid ^= (pgid >> 11);
	pgid += (pgid << 15);

	/* restrict to 31 bits */
	*pg_id_num = (pgid & 0x7fffffff);
}
