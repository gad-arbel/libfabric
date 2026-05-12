#!/bin/bash

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUTPUT_DIR="$SCRIPT_DIR/raw"
FILTERED_DIR="$SCRIPT_DIR/filtered"
CONF_DIR="$SCRIPT_DIR/conf"
PARTITION="2x-Fabrics"

run_affinity_test() {
	local prefix=$1
        # My envs
	export FI_VERBS_NIC_AFFINITY_POLICY=$2
	export FI_VERBS_AFFINITY_DEVICE=$3
	export FI_VERBS_NIC_AFFINITY_CONFIG=$4
        # Log
	export FI_LOG_LOCATION="$OUTPUT_DIR/${prefix}.log"
	export FI_LOG_LEVEL=debug
	export FI_LOG_PROV=verbs
	export FI_LOG_SUBSYS=core

	srun --partition=$PARTITION bash -c "module load hwloc && ~/.libfabric/bin/fi_info -p verbs" > "$OUTPUT_DIR/${prefix}.out"

	unset FI_VERBS_NIC_AFFINITY_POLICY FI_VERBS_AFFINITY_DEVICE FI_VERBS_NIC_AFFINITY_CONFIG 
        unset FI_LOG_LOCATION FI_LOG_LEVEL FI_LOG_PROV FI_LOG_SUBSYS
}

process_topo_file() {
	local input_file=$1
	local output_file=$2

	awk '
	# Stop processing when we hit the epilog (lines starting with "depth")
	/^depth/ { exit }

	/Machine/ || /Package/ || /NUMANode/ || /HostBridge/ || /PCIBridge/ || /PCI/ || /Network/ || /OpenFabrics/{
		if (/PCI/) {
			print $0
		}
		else {
			gsub(/\(.*\)/, "")
			print $0
		}
	}
	' "$input_file" > "$output_file"
}

process_out_file() {
	local input_file=$1
	local output_file=$2

	awk '
	/provider:/ {
		provider = $2
	}
	/domain:/ {
		domain = $2
		if (provider == "verbs" && domain != last_domain) {
			print domain
                        last_domain = domain
		}
	}
	' $input_file > $output_file
}

process_log_file() {
	local input_file=$1
	local output_file=$2

	grep "affinity" $input_file > $output_file
}

organize_all_outputs() {
	for file in "$OUTPUT_DIR"/*; do
		filename=$(basename "$file")

		if [[ "$filename" == *.out ]]; then
			process_out_file "$file" "$FILTERED_DIR/${filename}"
		elif [[ "$filename" == *.log ]]; then
			process_log_file "$file" "$FILTERED_DIR/${filename}"
		elif [[ "$filename" == *.topo ]]; then
                        process_topo_file "$file" "$FILTERED_DIR/${filename}"
		fi
	done
}

if [ -d "$OUTPUT_DIR" ]; then
    rm -rf "$OUTPUT_DIR"
fi
mkdir -p "$OUTPUT_DIR"
if [ -d "$FILTERED_DIR" ]; then
    rm -rf "$FILTERED_DIR"
fi
mkdir -p "$FILTERED_DIR"

srun --partition=$PARTITION bash -c "module load hwloc && lstopo --of console -v" &> $OUTPUT_DIR/topology.topo

# Addreses:
# Package0:
#       mlx5_0: 0000:5a:00.0
# Package1:
#       mlx5_1: 0000:d8:00.0

# run_affinity_test <prefix> <policy> <device> <config>
run_affinity_test "baseline" "" "" ""
run_affinity_test "none" "none" "" ""
run_affinity_test "manual_1" "manual" "0000:5a:00.0" "$CONF_DIR/config.conf"
run_affinity_test "manual_2" "manual" "0000:d8:00.0" "$CONF_DIR/config.conf"
run_affinity_test "auto_nodevice" "auto" "" ""
run_affinity_test "auto_mlx5_0" "auto" "0000:5a:00.0" ""
run_affinity_test "auto_mlx5_1" "auto" "0000:d8:00.0" ""

organize_all_outputs
