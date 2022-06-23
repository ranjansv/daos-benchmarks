#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/file.h>
#include <unistd.h>

#include "suite/daos_test.h"
#include <daos.h>
#include <daos/tests_lib.h>
#include <mpi.h>

#include <caliper/cali.h>
#include <caliper/cali-manager.h>

/** local task information */
int rank = -1;
int wrank;
int procs;
char node[128] = "unknown";

/* MPI communicator for writers */
MPI_Comm comm;

/** Name of the process set associated with the DAOS server */
#define DSS_PSETID "daos_server"
//#define	DSS_PSETID	 "daos_tier0"

#define MB_in_bytes 1048576
static daos_ofeat_t feat =
    DAOS_OF_DKEY_UINT64 | DAOS_OF_KV_FLAT | DAOS_OF_ARRAY;
static daos_ofeat_t featb =
    DAOS_OF_DKEY_UINT64 | DAOS_OF_KV_FLAT | DAOS_OF_ARRAY | DAOS_OF_ARRAY_BYTE;

/** Event queue */
daos_handle_t eq;

/** Pool information */
uuid_t pool_uuid;   /* only used on rank 0 */
d_rank_list_t svcl; /* only used on rank 0 */
daos_handle_t poh;  /* shared pool handle */

/** Container information */
uuid_t co_uuid;     /* only used on rank 0 */
daos_handle_t coh;  /* shared container handle */
daos_epoch_t epoch; /* epoch in-use */

/**
 * Array parameters
 * Each task overwrites a different section of the array at every iteration.
 * An epoch number is associated with each iteration. One task can have at
 * most MAX_IOREQS I/O requests in flight and then needs to wait for completion
 * of an request in flight before sending a new one.
 * The actual data written in the array is the epoch number.
 */
#define MAX_IOREQS 1 /* number of concurrent i/o reqs in flight */

/** an i/o request in flight */
struct io_req {

  d_iov_t iov;
  d_sg_list_t sg;

  daos_event_t ev;
};

/** data buffer */
char *data;

#define FAIL(fmt, ...)                                                         \
  do {                                                                         \
    fprintf(stderr, "Process %d(%s): " fmt " aborting\n", rank, node,          \
            ##__VA_ARGS__);                                                    \
    MPI_Abort(MPI_COMM_WORLD, 1);                                              \
  } while (0)

#define ASSERT(cond, ...)                                                      \
  do {                                                                         \
    if (!(cond))                                                               \
      FAIL(__VA_ARGS__);                                                       \
  } while (0)

static inline void ioreqs_init(struct io_req *reqs, size_t data_per_rank) {
  int rc;
  int j;

  for (j = 0; j < MAX_IOREQS; j++) {
    struct io_req *req = &reqs[j];

    /** initialize event */
    // rc = daos_event_init(&req->ev, eq, NULL);
    // ASSERT(rc == 0, "event init failed with %d", rc);

    /** initialize scatter/gather */
    req->iov = (d_iov_t) { .iov_buf = data,
                           .iov_buf_len = data_per_rank * sizeof(data[0]),
                           .iov_len = data_per_rank * sizeof(data[0]), };
    req->sg.sg_nr = 1;
    req->sg.sg_iovs = &req->iov;
  }
}

void array(size_t arr_size_mb, int steps) {
  daos_handle_t oh;
  struct io_req *reqs;
  int rc;
  int iter;

  size_t data_per_rank = arr_size_mb * MB_in_bytes / procs;

  /** allocate and initialize I/O requests */
  D_ALLOC_ARRAY(data, data_per_rank);
  D_ALLOC_ARRAY(reqs, MAX_IOREQS);
  ASSERT(reqs != NULL, "malloc of reqs failed");
  ioreqs_init(reqs, data_per_rank);

  /** Transactional overwrite of the array at each iteration */
  for (iter = 0; iter < steps; iter++) {

    MPI_Barrier(comm);
    if (rank == 0) {
      epoch++;
      daos_cont_create_snap(coh, &epoch, NULL, NULL);
      ASSERT(rc == 0, "daos_cont_create_snap failed with %d", rc);
    }
    MPI_Barrier(comm);
  }

  if (rank == 0)
    print_message("rank 0 array()..completed\n");

  D_FREE(reqs);
  D_FREE(data);
}

static void array_oh_share(daos_handle_t *oh) {
  d_iov_t ghdl = { NULL, 0, 0 };
  int rc;

  if (rank == 0) {
    /** fetch size of global handle */
    rc = daos_array_local2global(*oh, &ghdl);
    assert_rc_equal(rc, 0);
  }

  /** broadcast size of global handle to all peers */
  rc = MPI_Bcast(&ghdl.iov_buf_len, 1, MPI_UINT64_T, 0, MPI_COMM_WORLD);
  assert_int_equal(rc, MPI_SUCCESS);

  /** allocate buffer for global pool handle */
  D_ALLOC(ghdl.iov_buf, ghdl.iov_buf_len);
  ghdl.iov_len = ghdl.iov_buf_len;

  if (rank == 0) {
    /** generate actual global handle to share with peer tasks */
    rc = daos_array_local2global(*oh, &ghdl);
    assert_rc_equal(rc, 0);
  }

  /** broadcast global handle to all peers */
  rc = MPI_Bcast(ghdl.iov_buf, ghdl.iov_len, MPI_BYTE, 0, MPI_COMM_WORLD);
  assert_int_equal(rc, MPI_SUCCESS);

  if (rank != 0) {
    /** unpack global handle */
    rc = daos_array_global2local(coh, ghdl, 0, oh);
    assert_rc_equal(rc, 0);
  }

  D_FREE(ghdl.iov_buf);

  MPI_Barrier(MPI_COMM_WORLD);
}

// structure for message queue
typedef struct mesg_buffer {
  long mesg_type;
  char mesg_text[100];
} MesQ;

void write_data(size_t arr_size_mb, int steps, int async) {
  daos_obj_id_t oid;
  daos_handle_t oh;
  daos_array_iod_t iod;
  daos_range_t rg;
  d_sg_list_t sgl;
  d_iov_t iov;
  char *wbuf = NULL;
  daos_size_t i;
  daos_event_t ev, *evp;
  int rc;
  int iter;
  size_t num_elements;
  daos_size_t size;

  MesQ send_q;
  key_t key;
  int msgid;
  FILE *fp;
  char buf[100];
  int fd;

  /* Temporary assignment */
  daos_size_t cell_size = 1;
  //static daos_size_t chunk_size = 67108864;
  static daos_size_t chunk_size = 1048576;
  //static daos_size_t chunk_size = 2097152;
  // static daos_size_t chunk_size = 16;

  //MPI_Barrier(MPI_COMM_WORLD);
  /** create the array on rank 0 and share the oh. */
 
  CALI_MARK_BEGIN("daos_array-writer:oid-gen-n-share"); 
  if (rank == 0) {
    //oid = daos_test_oid_gen(coh, OC_SX, (cell_size == 1) ? featb : feat, 0, 0);
    oid.hi = 0;
    oid.lo = 3;
    daos_array_generate_oid(coh, &oid, true, 0, 0, 0);
    rc = daos_array_create(coh, oid, DAOS_TX_NONE, cell_size, chunk_size, &oh,
                           NULL);
    assert_rc_equal(rc, 0);

    fp = fopen("./share/oid_lo.txt", "w");
    fprintf(fp, "%lu", oid.lo);
    fclose(fp);

    fp = fopen("./share/oid_hi.txt", "w");
    fprintf(fp, "%lu", oid.hi);
    fclose(fp);

    fp = fopen("./share/oid_part_count.txt", "w");
    fd = fileno(fp);
    if (flock(fd, LOCK_EX) == -1)
      exit(1);
    fprintf(fp, "%d", 2);
    fclose(fp);

    printf("oid.lo = %lu, oid.hi = %lu\n", oid.lo, oid.hi); 
  }
  array_oh_share(&oh);
  CALI_MARK_END("daos_array-writer:oid-gen-n-share"); 

  /** Allocate and set buffer */
  // num_elements = arr_size_mb ;
  if (rank == 0)
    printf("arr_size_mb = %d\n", arr_size_mb);
  num_elements = arr_size_mb * MB_in_bytes / procs;
  D_ALLOC_ARRAY(wbuf, num_elements);
  assert_non_null(wbuf);

  for (i = 0; i < num_elements; i++)
    wbuf[i] = i + 1;

  /** set array location */
  iod.arr_nr = 1;
  rg.rg_len = num_elements * sizeof(char) / cell_size;
  rg.rg_idx = rank * rg.rg_len;
  iod.arr_rgs = &rg;

  /** set memory location */
  sgl.sg_nr = 1;
  d_iov_set(&iov, wbuf, num_elements * sizeof(char));
  sgl.sg_iovs = &iov;

  daos_handle_t th;
  char snapshot_name[50];

  int num_snapshots = 0;
  daos_epoch_t epochs[steps];
  char list_snapnames[steps][50];
  daos_anchor_t anchor;
  memset(&anchor, 0, sizeof(anchor));

  CALI_MARK_BEGIN("daos_array-writer:iterations");

  for (iter = 0; iter < steps; iter++) {
    /** Write */
    // if (async) {
    //  rc = daos_event_init(&ev, eq, NULL);
    //  assert_rc_equal(rc, 0);
    //}
    // rc = daos_tx_open(coh, &th, 0, NULL);
    // assert_rc_equal(rc, 0);
    // rc = daos_array_write(oh, th, &iod, &sgl, async ? &ev : NULL);
    CALI_MARK_BEGIN("daos_array-writer:write-time-outside-barrier");
    MPI_Barrier(MPI_COMM_WORLD);
    CALI_MARK_BEGIN("daos_array-writer:write-time-inside-barrier");
    rc = daos_array_write(oh, DAOS_TX_NONE, &iod, &sgl, async ? &ev : NULL);
    assert_rc_equal(rc, 0);
    CALI_MARK_END("daos_array-writer:write-time-inside-barrier");
    MPI_Barrier(MPI_COMM_WORLD);
    CALI_MARK_END("daos_array-writer:write-time-outside-barrier");
    // rc = daos_tx_commit(th, NULL);
    // assert_rc_equal(rc, 0);
    // rc = daos_tx_close(th, NULL);
    // assert_rc_equal(rc, 0);

    CALI_MARK_BEGIN("daos_array-writer:snapshot-time");
    if (rank == 0) {
      // sprintf(snapshot_name, "snapshot-", iter + 1);
      // rc = daos_cont_create_snap(coh, &epoch, snapshot_name, NULL);
      rc = daos_cont_create_snap(coh, &epoch, NULL, NULL);

      printf("daos_cont_create_snap, rc = %d\n", rc);
      printf("epoch = %lu\n", epoch);
      ASSERT(rc == 0, "daos_cont_create_snap failed with %d", rc);


      sprintf(buf, "./share/container-snap-%d.txt", iter);
      fp = fopen(buf, "w");
      fprintf(fp, "%lu", epoch);
      fclose(fp);

      fp = fopen("./share/snapshot_count.txt","w");
      fd = fileno(fp);
      if (flock(fd, LOCK_EX) == -1)
        exit(1);
      fprintf(fp, "%d", iter + 1);
      fclose(fp);
    }
    MPI_Barrier(MPI_COMM_WORLD);
    CALI_MARK_END("daos_array-writer:snapshot-time");
  }
  CALI_MARK_END("daos_array-writer:iterations");

  if (rank == 0) {
    rc = daos_array_get_size(oh, DAOS_TX_NONE, &size, NULL);
    ASSERT(rc == 0, "daos_array_get_size failed with %d", rc);
    printf("Array size = %lu\n", size);

    rc = daos_cont_list_snap(coh, &num_snapshots, epochs, list_snapnames,
                             &anchor, NULL);
    ASSERT(rc == 0, "daos_cont_list_snap failed with %d", rc);

    printf("Number of snapshots: %d\n", num_snapshots);
  }
  D_FREE(wbuf);
}

int main(int argc, char **argv) {
  int rc;
  uuid_parse(argv[1], pool_uuid);
  uuid_parse(argv[2], co_uuid);
  size_t arr_size_mb = strtol(argv[3],NULL,10);
  int steps = strtol(argv[4],NULL,10);

  rc = gethostname(node, sizeof(node));
  ASSERT(rc == 0, "buffer for hostname too small");

  cali_config_set("CALI_CALIPER_ATTRIBUTE_DEFAULT_SCOPE", "process");

  rc = MPI_Init(&argc, &argv);
  ASSERT(rc == MPI_SUCCESS, "MPI_Init failed with %d", rc);

  MPI_Comm_size(MPI_COMM_WORLD, &wrank);

  const unsigned int color = 1;
  MPI_Comm_split(MPI_COMM_WORLD, color, wrank, &comm);

  MPI_Comm_rank(comm, &rank);
  MPI_Comm_size(comm, &procs);

  /** initialize the local DAOS stack */
  rc = daos_init();
  ASSERT(rc == 0, "daos_init failed with %d", rc);

  /** create event queue */
  rc = daos_eq_create(&eq);
  ASSERT(rc == 0, "eq create failed with %d", rc);

  CALI_MARK_BEGIN("daos_array-writer:pool_connect");
  if (rank == 0) {
    /** create a test pool and container for this test */
    // pool_create();

    /** connect to the just created DAOS pool */
    rc = daos_pool_connect(pool_uuid, DSS_PSETID,
                           // DAOS_PC_EX ,
                           DAOS_PC_RW /* read write access */,
                           &poh /* returned pool handle */,
                           NULL /* returned pool info */, NULL /* event */);
    ASSERT(rc == 0, "pool connect failed with %d", rc);
  }

  /** share pool handle with peer tasks */
  handle_share(&poh, HANDLE_POOL, rank, poh, 1);
  CALI_MARK_END("daos_array-writer:pool_connect");
 
  CALI_MARK_BEGIN("daos_array-writer:cont_connect");
  if (rank == 0) {
    /** generate uuid for container */
    // uuid_generate(co_uuid);
    /** create container */
    // rc = daos_cont_create(poh, co_uuid, NULL /* properties */,
    //                      NULL /* event */);
    // ASSERT(rc == 0, "container create failed with %d", rc);

    /** open container */
    rc = daos_cont_open(poh, co_uuid, DAOS_COO_RW, &coh, NULL, NULL);
    ASSERT(rc == 0, "container open failed with %d", rc);
  }

  /** share container handle with peer tasks */
  handle_share(&coh, HANDLE_CO, rank, poh, 1);
  CALI_MARK_END("daos_array-writer:cont_connect");

  /** the other tasks write the array */
  // array(arr_size_mb, steps);
  write_data(arr_size_mb, steps, 0 /* Async I/O flag False*/);

  /** close container */
  daos_cont_close(coh, NULL);

  /** disconnect from pool & destroy it */
  daos_pool_disconnect(poh, NULL);
  // if (rank == 0)
  /** free allocated storage */
  //	pool_destroy();

  /** destroy event queue */
  rc = daos_eq_destroy(eq, 0);
  ASSERT(rc == 0, "eq destroy failed with %d", rc);

  /** shutdown the local DAOS stack */
  rc = daos_fini();
  ASSERT(rc == 0, "daos_fini failed with %d", rc);

  MPI_Finalize();
  return rc;
}
