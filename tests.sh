#!/bin/bash

# increase soft file descriptor limit
#ulimit -S 10032

if [[ $# -gt 0 ]]; then
    port=$1
else
    port=$(( ( RANDOM % 50000 ) + 10000 ))
    echo "using port $port"
fi

testdir=./testdir

red='\033[0;31m'
green='\033[0;32m'
reset='\x1b[0m'

function cleanup_exit {
    if pgrep relay ; then
        kill -9 $(pgrep relay)
    fi
}

function cleanup_failure {
    echo -e "${red}Tests failed!${reset}"
    kill -9 $(pgrep relay)
}

function cleanup_success {
    echo -e "${green}Tests passed!${reset}"
    kill -2 $(pgrep relay)
}

trap 'cleanup_exit' EXIT

function run_tests {
    set -e
    rm -rf "$testdir"
    mkdir -p "$testdir"/in "$testdir"/out
    passed=1

    ./relay :$port > "$testdir"/relay.log 2>&1 &

    echo "Generating test data..."
    y=0
    for x in $(seq 100 250 125000); do
        y=$(( y + 1 ))
        echo " test_$y.dat with size $x"
        dd count=$x if=/dev/urandom of="$testdir"/in/test_$y.dat > /dev/null 2>&1
    done
    testcount=$y

    #run all the sends
    echo "Running all sends..."
    rm -f "$testdir"/secrets.txt
    for y in $(seq 1 $testcount); do
        ./send localhost:$port "$testdir"/in/test_$y.dat >> "$testdir"/secrets.txt &
    done
    sleep 10
    #wait for secrets to be available
    while [[ $(wc -l "$testdir"/secrets.txt | cut -d" " -f1) -lt $testcount ]]; do
        sleep 1
    done
    #run all the receives
    echo "Running all receives..."
    for secret in $(cat "$testdir"/secrets.txt); do
        ./receive localhost:$port "$secret" "$testdir"/out &
    done

    while true; do
        sends=$(pgrep send | wc -l)
        recvs=$(pgrep receive | wc -l)
        if [[ $sends -eq 0 && $recvs -eq 0 ]]; then
            break
        fi
        echo "Waiting for sends/receives..."
        sleep 5
    done

    #verify the results
    echo "Verifying all data transferred correctly..."
    for y in $(seq 1 $testcount); do
        insum=$(md5sum "$testdir"/in/test_$y.dat | awk '{print $1}')
        outsum=$(md5sum "$testdir"/out/test_$y.dat | awk '{print $1}')
        if [[ "$insum" == "$outsum" ]]; then
            echo -e "Copy passed: $y"
        else
            echo -e "${red}Copy failed: $y${reset}"
            passed=0
        fi
    done
}

run_tests
if [[ $passed -gt 0 ]]; then
    cleanup_success
else
    cleanup_failure
fi
