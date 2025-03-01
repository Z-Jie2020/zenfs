#!/bin/bash
set -e

SCRIPT_DIR=$(cd $(dirname "${BASH_SOURCE[0]}") && pwd)
cd $SCRIPT_DIR

function print_duration() {
  SECS=$1
  HRS=$(($SECS / 3600))
  SECS=$(($SECS % 3600))
  MINS=$(($SECS / 60))
  SECS=$(($SECS % 60))
  echo "$HRS"h "$MINS"m "$SECS"s
}

NAME=$1
TEST_DIR=$2
TESTS=$(ls $TEST_DIR/*_*.sh)

OK_TESTS=0
FAILED_TESTS=0

TOOLS_DIR=${TOOLS_DIR:-"../../../"}
ZENFS_DIR=${ZENFS_DIR:-"../util/"}
STRESS_CMD="../../../db_stress"

export TOOLS_DIR
export ZENFS_DIR
export STRESS_CMD

OUTPUT_DIR=${OUTPUT_DIR:-results}
RESULT_PATH="${OUTPUT_DIR}/$NAME"
RESULT_DIR="${RESULT_PATH}/$TEST_DIR"
echo "Results will be stored in $RESULT_DIR"

mkdir -p $RESULT_DIR
rm -rf $RESULT_DIR/*

# Seconds is a bash variable that increments every second
# See https://www.gnu.org/software/bash/manual/html_node/Bash-Variables.html
SECONDS=0

for TEST in $TESTS
do
  TESTCASE="$TEST"
  echo "$(tput setaf 3)Running $TEST $(tput sgr 0)"
  
  export RESULT_DIR="$RESULT_DIR"
  export TEST_OUT="$RESULT_PATH/${TEST/.sh/.out}"
  START_SECONDS=$SECONDS
  
  if [ -v PRE_CMD ]; then
    set +e
    $PRE_CMD > "$RESULT_PATH/${TEST/.sh/.PRE_CMD.out}"
    set -e
  fi

  set +e
  $TESTCASE
  RES=$?
  set -e

  echo ""
  echo "Test duration $(print_duration $(($SECONDS - $START_SECONDS)))" | tee -a $TEST_OUT
  if [ $RES -eq 0 ]; then
    echo "$(tput setaf 2)OK$(tput sgr 0)"
    OK_TESTS=$((OK_TESTS+1))
  else
    echo "$(tput setaf 1)FAILED$(tput sgr 0)"
    FAILED_TESTS=$((FAILED_TESTS+1))
  fi

  if [ -v LOG_FILE_PATH ]; then
    set +e
    cp $LOG_FILE_PATH "$RESULT_PATH/${TEST/.sh/.LOG}"
    set -e
  fi

  if [ -v POST_CMD ]; then
    set +e
    $POST_CMD > "$RESULT_PATH/${TEST/.sh/.POST_CMD.out}"
    set -e
  fi

done

echo ""

if [ $FAILED_TESTS -eq 0 ]; then
  echo "$(tput setaf 2)ALL TESTS PASSED$(tput sgr 0)"
else
  echo "$(tput setaf 1)$FAILED_TESTS TESTS FAILED$(tput sgr 0)"
fi

echo "Test set duration $(print_duration $SECONDS)" | tee $RESULT_DIR/test_set_duration
echo "Test set output available at $RESULT_DIR"

exit $FAILED_TESTS
