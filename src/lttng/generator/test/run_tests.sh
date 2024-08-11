set -eu

FILE_PATH=`realpath $0`
SCRIPT_DIR=`dirname ${FILE_PATH}`

echo "Running test_traces_output.sh..."
${SCRIPT_DIR}/test_traces_output.sh

echo "Running test_failures.sh..."
${SCRIPT_DIR}/../failure_tests/test_failures.py
