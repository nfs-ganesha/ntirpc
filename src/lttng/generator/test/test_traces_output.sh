#!/bin/bash

set -eu

FILE_PATH=`realpath $0`
SCRIPT_DIR=`dirname ${FILE_PATH}`
SESSION_NAME="LTTNG_GENERATOR_TEST_SESSION"
CHANNEL_NAME="LTTNG_GENERATOR_TEST_CHANNEL"
TRACES_OUT_DIR=`mktemp -d`
TRACES_OUT="${TRACES_OUT_DIR}/test_traces"

print_help_and_fail() {
	echo "$0 [--generate]"
	echo "    --generate overwrites the ground truth files with new ones"
	exit 1
}

generate_lttng_trace() {
	input=$1
	output=$2
	babeltrace2 --no-delta ${input} | grep "format" > ${output}

	# Remove cpu_id
	sed -i 's\{ cpu_id = [0-9]* }, \\g' ${output}

	# Remove timestamp
	# Example of a timestamp: [16:56:32.087900151]
	sed -E -i 's/\[[:.0-9]+\] //g' ${output}

	# Remove hostname
	sed -E -i 's/^[^ ]* //g' ${output}
}

generate_formatted_trace() {
	input=$1
	output=$2
	${SCRIPT_DIR}/../trace_formatter.py $input > $output

	# Remove timestamp.
	# Example of a timestamp: [16:56:32.087900151]
	sed -E -i 's/[-0-9]+\s[:.0-9]+\s\|\s//g' ${output}

	# Remove cpu_id
	sed -E -i 's/cpu_id=[0-9]+\s\|\s//g' ${output}
}

if [[ $# -ge 2 ]]; then
  print_help_and_fail
fi

generating=false
if [[ $# -eq 1 ]]; then
	if [ $1 != "--generate" ]; then
		print_help_and_fail
	fi
	generating=true
fi

# Cleanup in case of leftovers
lttng destroy ${SESSION_NAME} &> /dev/null || true
rm -rf ${TRACES_OUT_DIR}

mkdir ${TRACES_OUT_DIR}
lttng create --output=${TRACES_OUT} ${SESSION_NAME}
lttng enable-event -u -a --channel=${CHANNEL_NAME}
lttng start ${SESSION_NAME}

echo "Running test binary"
${SCRIPT_DIR}/test_bin

lttng destroy ${SESSION_NAME}

if [ $generating = true ]; then
	echo "Generating results..."
	generate_lttng_trace ${TRACES_OUT} ${SCRIPT_DIR}/expected_lttng.log
	generate_formatted_trace ${TRACES_OUT} ${SCRIPT_DIR}/expected_formated.log

	echo "Done! Now check to see if it looks good and commit the changes"
	exit 0
fi

echo "Generating traces in ${TRACES_OUT_DIR}"
generate_lttng_trace ${TRACES_OUT} ${TRACES_OUT_DIR}/result_lttng.log
generate_formatted_trace ${TRACES_OUT} ${TRACES_OUT_DIR}/formatted_results.log

echo "Verifying lttng results..."
diff ${TRACES_OUT_DIR}/result_lttng.log ${SCRIPT_DIR}/expected_lttng.log

echo "Verifying formated results..."
diff ${TRACES_OUT_DIR}/formatted_results.log ${SCRIPT_DIR}/expected_formated.log

echo "Test passed!"


