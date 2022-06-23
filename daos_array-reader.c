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
#define NUM_OBJS 1

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

void shuffle(int *array, size_t n)
{
    if (n > 1) 
    {
        size_t i;
        for (i = 0; i < n - 1; i++) 
        {
          size_t j = i + rand() / (RAND_MAX / (n - i) + 1);
          int t = array[j];
          array[j] = array[i];
          array[i] = t;
        }
    }
}

void read_data(size_t arr_size_mb, size_t iosize_bytes, int steps, int async, int flag_random_read) {
  daos_obj_id_t oid;
  daos_handle_t oh;
  daos_handle_t th;
  daos_array_iod_t iod;
  daos_range_t *rg;
  d_sg_list_t sgl;
  d_iov_t iov;
  char *rbuf = NULL;
  daos_size_t i;
  daos_event_t ev, *evp;
  int rc;
  int iter;
  size_t elements_per_rank;
  daos_size_t size;
  char *eptr;

  MesQ recv_q;
  key_t key;
  int msgid;
  FILE *fp;
  char buf[100];

  /* Temporary assignment */
  daos_size_t cell_size = 1;
  //static daos_size_t chunk_size = 67108864;
  static daos_size_t chunk_size = 1048576;
  //static daos_size_t chunk_size = 2097152;
  // static daos_size_t chunk_size = 16;
  daos_obj_id_t oids[NUM_OBJS];
  uint32_t oids_nr;
  daos_anchor_t anchor;

  int num_snapshots;
  char list_snapnames[steps][50];
  daos_epoch_t epochs[steps];
  int fd;

  int num_committed_snapshots = 0;
  int oid_part_count = 0;

  oids_nr = 0;
  for (iter = 0; iter < NUM_OBJS; iter++)
    memset(&oids[iter], 0, sizeof(daos_obj_id_t));

  if (rank == 0) {
    printf("arr_size_mb = %d\n", arr_size_mb);

    while(oid_part_count != 2) {
      usleep(10000);
      fp = fopen("./share/oid_part_count.txt", "r");
      fd = fileno(fp);
      if (flock(fd, LOCK_EX) == -1)
        exit(1);
      fscanf(fp, "%d", &oid_part_count);
      fclose(fp);
    }
    fp = fopen("share/oid_lo.txt", "r");
    fscanf(fp, "%lu", &oid.lo);
    fclose(fp);

    fp = fopen("share/oid_hi.txt", "r");
    fscanf(fp, "%lu", &oid.hi);
    fclose(fp);
    printf("rank = 0, oid.lo = %lu, oid.hi = %lu\n", oid.lo, oid.hi);
    // msgctl(msgid, IPC_RMID, NULL);
  }
  rc = MPI_Bcast(&oid.lo, 1, MPI_UINT64_T, 0, MPI_COMM_WORLD);
  assert_int_equal(rc, MPI_SUCCESS);
  rc = MPI_Bcast(&oid.hi, 1, MPI_UINT64_T, 0, MPI_COMM_WORLD);
  assert_int_equal(rc, MPI_SUCCESS);

  if (rank) {
    printf("rank = %d, oid.lo = %lu, oid.hi = %lu\n", rank, oid.lo, oid.hi);
  }

  elements_per_rank = arr_size_mb * MB_in_bytes / procs;
  D_ALLOC_ARRAY(rbuf, elements_per_rank);
  assert_non_null(rbuf);

  /** set array location */
  iod.arr_nr = elements_per_rank/iosize_bytes;
  rg = (daos_range_t *) malloc(iod.arr_nr * sizeof(daos_range_t));
  daos_off_t start_index = rank * elements_per_rank / sizeof(char);
  daos_size_t read_length = sizeof(char) * iosize_bytes;

  //Array of offsets to choose at random. This array will track offset chosen
  //so that we don't pick them again
  int arr_offsets[iod.arr_nr];

  for(iter = 0; iter < iod.arr_nr; iter++)
      arr_offsets[iter] = iter;

  if(flag_random_read == 1)
  shuffle(arr_offsets,iod.arr_nr);

  for(iter = 0; iter < iod.arr_nr; iter++) { 
     rg[iter].rg_len = read_length;
     rg[iter].rg_idx = start_index + arr_offsets[iter] * iosize_bytes;
  }
  iod.arr_rgs = rg;

  if(rank == 0) {
    printf("arr_nr = %lu\n", iod.arr_nr);
    printf("start_index = %lu\n", start_index);
    printf("read_length = %lu\n", read_length);
  }

  /** set memory location */
  sgl.sg_nr = 1;
  d_iov_set(&iov, rbuf, elements_per_rank * sizeof(char));
  sgl.sg_iovs = &iov;

  CALI_MARK_BEGIN("daos_array-reader:iterations");

  for (iter = 0; iter < steps; iter++) {
    //MPI_Barrier(MPI_COMM_WORLD);

    CALI_MARK_BEGIN("daos_array-reader:get_epochid");
    if (rank == 0) {
      printf("Waiting to read epoch of snapshot %d\n", iter + 1);

      while (iter + 1 > num_committed_snapshots) {
        usleep(10000);
        fp = fopen("./share/snapshot_count.txt", "r");
        fd = fileno(fp);
        if (flock(fd, LOCK_EX) == -1)
          exit(1);
        fscanf(fp, "%lu", &num_committed_snapshots);
        fclose(fp);
      }

      sprintf(buf, "share/container-snap-%d.txt", iter);
      fp = fopen(buf, "r");
      fscanf(fp, "%lu", &epochs[iter]);
      fclose(fp);
    }
    // MPI share epoch
    rc = MPI_Bcast(&epochs[iter], 1, MPI_UINT64_T, 0, MPI_COMM_WORLD);
    assert_int_equal(rc, MPI_SUCCESS);
    CALI_MARK_END("daos_array-reader:get_epochid");

    printf("iter %d rank %d epoch: %lu\n", iter + 1, rank, epochs[iter]);

    CALI_MARK_BEGIN("daos_array-reader:open_snap");
    rc = daos_tx_open_snap(coh, epochs[iter], &th, NULL);
    ASSERT(rc == 0, "daos_tx_open_snap failed with %d", rc);
    CALI_MARK_END("daos_array-reader:open_snap");

    CALI_MARK_BEGIN("daos_array-reader:open_array");
    rc = daos_array_open(coh, oid, th, DAOS_OO_RW, &cell_size, &chunk_size, &oh,
                         NULL);
    ASSERT(rc == 0, "daos_array_open failed with %d", rc);
    CALI_MARK_END("daos_array-reader:open_array");

    CALI_MARK_BEGIN("daos_array-reader:read-time");
    rc = daos_array_read(oh, th, &iod, &sgl, NULL);
    ASSERT(rc == 0, "daos_array_read failed with %d", rc);
    CALI_MARK_END("daos_array-reader:read-time");

    CALI_MARK_BEGIN("daos_array-reader:close_array");
    rc = daos_array_close(oh, NULL);
    ASSERT(rc == 0, "daos_array_close failed with %d", rc);
    CALI_MARK_END("daos_array-reader:close_array");

    // rc = daos_oit_open(coh, epochs[iter], &oh, NULL);
    // ASSERT(rc == 0, "daos_oit_open failed with %d", rc);

    // memset(&anchor, 0, sizeof(anchor));
    // rc = daos_oit_list(oh, oids, &oids_nr, &anchor, NULL);
    // ASSERT(rc == 0, "daos_oit_list failed with %d", rc);

    // rc = daos_oit_close(oh, NULL);
    // ASSERT(rc == 0, "daos_oit_close failed with %d", rc);

    MPI_Barrier(MPI_COMM_WORLD);
  }
  CALI_MARK_END("daos_array-reader:iterations");

  D_FREE(rbuf);
}

int main(int argc, char **argv) {
  int rc;
  uuid_parse(argv[1], pool_uuid);
  uuid_parse(argv[2], co_uuid);
  size_t arr_size_mb = strtol(argv[3],NULL,10);
  size_t iosize_bytes = strtol(argv[4],NULL,10);
  int steps = strtol(argv[5],NULL,10);
  int flag_random_read;

  if(strcmp(argv[6],"random") == 0)
      flag_random_read = 1;
  else
     flag_random_read = 0;

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

   
  CALI_MARK_BEGIN("daos_array-reader:pool_connect");
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
  CALI_MARK_END("daos_array-reader:pool_connect");

  CALI_MARK_BEGIN("daos_array-reader:cont_connect");
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
  CALI_MARK_END("daos_array-reader:cont_connect");

  /** the other tasks write the array */
  // array(arr_size_mb, steps);
  read_data(arr_size_mb, iosize_bytes, steps, 0 /* Async I/O flag False*/, flag_random_read);

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
