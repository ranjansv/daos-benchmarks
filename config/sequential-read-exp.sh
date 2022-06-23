BENCH_TYPE="writer-reader"
ENGINE="daos-array ior+dfs"
#Data per writer rank per iteration in MB
DATA_PER_RANK="64"
#Read IO size in bytes
READ_IO_SIZE="1024"
READ_PATTERN="sequential"
STEPS=8
#Number of writer ranks
PROCS="28"
#Number of reader ranks = $PROCS/$READ_WRITE_RATIO
READ_WRITE_RATIO=1
