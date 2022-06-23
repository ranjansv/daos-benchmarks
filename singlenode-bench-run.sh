POOL_UUID=$(dmg -i -o $daos_config pool list --verbose | grep ranjan | awk '{print $2}')
echo "POOL_UUID: $POOL_UUID"
echo "SLURM_JOB_NUM_NODES: $SLURM_JOB_NUM_NODES"
SLURM_JOB_CPUS_PER_NODE=28
echo "SLURM_JOB_CPUS_PER_NODE: $SLURM_JOB_CPUS_PER_NODE"

CONFIG_FILE=$1

#Load config file
source ${CONFIG_FILE}

#Create timestamped output directory
TIMESTAMP=$(echo $(date +%Y-%m-%d-%H:%M:%S))
RESULT_DIR="results/$TIMESTAMP"
mkdir -p $RESULT_DIR

echo "Result dir: $RESULT_DIR"

rm results/latest
ln -s $TIMESTAMP results/latest

#mount > $RESULT_DIR/fs-mounts.log
git branch --show-current >$RESULT_DIR/git.log
git log --format="%H" -n 1 >>$RESULT_DIR/git.log

#Build source
mkdir -p build
cd build
make clean && make
cd ..

#Copy configs and xml to outputdir
cp ${CONFIG_FILE} $RESULT_DIR/config.sh

SCRIPT_NAME="singlenode-bench-run.sh"
cp ./$SCRIPT_NAME $RESULT_DIR/

export I_MPI_PIN=0
export I_MPI_ROOT=/opt/intel/compilers_and_libraries_2020.4.304/linux/mpi
export TACC_MPI_GETMODE=impi_hydra

is_daos_agent_running=$(pgrep daos_agent)
echo $is_daos_agent_running
if [[ $is_daos_agent_running -eq "" ]]; then
	export TACC_TASKS_PER_NODE=1
	ibrun -np $SLURM_JOB_NUM_NODES ./daos_startup.sh &
	unset TACC_TASKS_PER_NODE
else
	echo "daos_agent is already running"
fi

echo "Waiting for agent to initialize..."
sleep 6

echo "benchmark type: $BENCH_TYPE"

for NR in $PROCS; do
	offset=$NR

	NR_READERS=$(echo "scale=0; $NR/$READ_WRITE_RATIO" | bc)

	for IO_NAME in $ENGINE; do
		#Parse IO_NAME for engine and storage type in case of DAOS
		if grep -q "ior+dfs" <<<"$IO_NAME"; then
			ENG_TYPE="ior+dfs"
			FILENAME="/testfile"
		elif grep -q "daos-array" <<<"$IO_NAME"; then
			ENG_TYPE="daos-array"
			FILENAME="N/A"
		fi

		for DATASIZE in $DATA_PER_RANK; do
			echo ""
			echo ""
			echo "Processing ${NR} writers , ${ENG_TYPE}:${FILENAME}, ${DATASIZE}mb"
			#Choose PROCS and STEPS so that global array size is a whole numebr
			GLOBAL_ARRAY_SIZE=$(echo "scale=0; $DATASIZE * ($NR)" | bc)
			echo "global array size: $GLOBAL_ARRAY_SIZE"

			if [ $ENG_TYPE == "daos-array" ]; then
				echo "Destroying previous containers, if any "
				daos pool list-cont --pool=$POOL_UUID | sed -e '1,2d' | awk '{print $1}' | xargs -L 1 -I '{}' sh -c "daos cont destroy --cont={} --pool=$POOL_UUID --force"

				#Delete share directory contents with previous epoch values
				mkdir -p share/
				rm -rf share/*
				echo "0" >share/snapshot_count.txt
				echo "0" >share/oid_part_count.txt
				CONT_UUID=$(daos cont create --pool=$POOL_UUID | grep -i 'created container' | awk '{print $4}')
				echo "New container UUID: $CONT_UUID"
				OUTPUT_DIR="$RESULT_DIR/${NR}ranks/${DATASIZE}mb/${IO_NAME}/"
				mkdir -p $OUTPUT_DIR
				if [ $BENCH_TYPE == "writer-reader" ]; then
					START_TIME=$SECONDS
					ibrun -o 0 -n $NR numactl --cpunodebind=0 --preferred=0 env CALI_CONFIG=runtime-report,calc.inclusive build/daos_array-writer $POOL_UUID $CONT_UUID $GLOBAL_ARRAY_SIZE $STEPS &>>$OUTPUT_DIR/stdout-mpirun-writers.log
					ELAPSED_TIME=$(($SECONDS - $START_TIME))
					echo "$ELAPSED_TIME" >$OUTPUT_DIR/writeworkflow-time.log

					#read -n 1 -r -s -p $'Press enter to continue...\n'

					for IOSIZE in $READ_IO_SIZE; do
						echo "Starting readers with read io size(bytes): $IOSIZE"
						START_TIME=$SECONDS
						ibrun -o 0 -n $NR_READERS numactl --cpunodebind=0 --preferred=0 env CALI_CONFIG=runtime-report,calc.inclusive build/daos_array-reader $POOL_UUID $CONT_UUID $GLOBAL_ARRAY_SIZE $IOSIZE $STEPS $READ_PATTERN &>>$OUTPUT_DIR/stdout-mpirun-readers-iosize-$IOSIZE.log
						ELAPSED_TIME=$(($SECONDS - $START_TIME))
						echo "$ELAPSED_TIME" >$OUTPUT_DIR/readworkflow-iosize-${IOSIZE}-time.log
					done
				fi

			elif [ $ENG_TYPE == "ior+dfs" ]; then
				echo "Destroying previous containers, if any "
				daos pool list-cont --pool=$POOL_UUID | sed -e '1,2d' | awk '{print $1}' | xargs -L 1 -I '{}' sh -c "daos cont destroy --cont={} --pool=$POOL_UUID --force"
				CONT_UUID=$(daos cont create --pool=$POOL_UUID --type=POSIX | grep -i 'created container' | awk '{print $4}')
				echo "New POSIX container UUID: $CONT_UUID"
				OUTPUT_DIR="$RESULT_DIR/${NR}ranks/${DATASIZE}mb/${IO_NAME}/"
				mkdir -p $OUTPUT_DIR
				if [ $BENCH_TYPE == "writer-reader" ]; then
					echo "Starting IOR writers"
					START_TIME=$SECONDS
					ibrun -o 0 -n $NR numactl --cpunodebind=0 --preferred=0 ior -a DFS -b ${DATASIZE}mb -t ${DATASIZE}mb -v -W -w -k -o $FILENAME --dfs.pool $POOL_UUID --dfs.cont $CONT_UUID &>>$OUTPUT_DIR/stdout-mpirun-writers.log
					ELAPSED_TIME=$(($SECONDS - $START_TIME))
					echo "$ELAPSED_TIME" >$OUTPUT_DIR/writeworkflow-time.log

					TOTAL_READ_DATA=$(echo "scale=0; $DATASIZE * $READ_WRITE_RATIO" | bc)
					for IOSIZE in $READ_IO_SIZE; do
						echo "Starting IOR readers with read io size(bytes): $IOSIZE"
						if [ $READ_PATTERN == "sequential" ]
						then
						    START_TIME=$SECONDS
						    ibrun -o 0 -n $NR_READERS numactl --cpunodebind=0 --preferred=0 ior -a DFS -b ${TOTAL_READ_DATA}mb -t $IOSIZE -v -r -i $STEPS -k -o $FILENAME --dfs.pool $POOL_UUID --dfs.cont $CONT_UUID &>>$OUTPUT_DIR/stdout-mpirun-readers-iosize-$IOSIZE.log
						    ELAPSED_TIME=$(($SECONDS - $START_TIME))
						else
						    START_TIME=$SECONDS
						    ibrun -o 0 -n $NR_READERS numactl --cpunodebind=0 --preferred=0 ior -a DFS -b ${TOTAL_READ_DATA}mb -t $IOSIZE -v -z -r -i $STEPS -k -o $FILENAME --dfs.pool $POOL_UUID --dfs.cont $CONT_UUID &>>$OUTPUT_DIR/stdout-mpirun-readers-iosize-$IOSIZE.log
						    ELAPSED_TIME=$(($SECONDS - $START_TIME))
						fi
						echo "$ELAPSED_TIME" >$OUTPUT_DIR/readworkflow-iosize-$IOSIZE-time.log
					done
				else
					echo "ior+dfs is not currently setup for concurrent readers and writers"
					continue
				fi
				echo "Cleaning up daos-posix container"
				daos pool list-cont --pool=$POOL_UUID | sed -e '1,2d' | awk '{print $1}' | xargs -L 1 -I '{}' sh -c "daos cont destroy --cont={} --pool=$POOL_UUID --force"
			fi

		done
	done
done

echo "Destroying previous containers, if any "
daos pool list-cont --pool=$POOL_UUID | sed -e '1,2d' | awk '{print $1}' | xargs -L 1 -I '{}' sh -c "daos cont destroy --cont={} --pool=$POOL_UUID --force"

echo "Generating CSV files"
./parse-result.sh $RESULT_DIR

echo "List of stdout files with error"
find $RESULT_DIR/ -iname 'stdout*.log' | xargs ls -1t | tac | xargs grep --color='auto' -ilE 'error|termination|fail'

find $RESULT_DIR/ -iname 'compareread*.csv' | xargs tail
if [ $BENCH_TYPE == "workflow" ]; then
	echo ""
	echo "Workflow times"
	find $RESULT_DIR/ -iname 'workflow*.csv' | xargs tail
fi
#source ./daos-list-cont.sh
