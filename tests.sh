#!/bin/bash

# increase soft file descriptor limit
#ulimit -n 10032

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

#trap 'cleanup_failure' SIGTERM SIGINT SIGSEGV SIGABRT
trap 'cleanup_exit' EXIT

function run_tests {
    set -e
    rm -rf "$testdir"
    mkdir -p "$testdir"/in "$testdir"/out
    ret=0

    ./relay :$port &# > "$testdir"/relay.log 2>&1 &

    #generate sample data from 100 bytes to 10MB
    y=0
    for x in $(seq 100 5000 100000); do
        y=$(( y + 1 ))
        dd count=$x if=/dev/urandom of="$testdir"/in/in_$y.dat > /dev/null 2>&1
    done

    #run all the sends
    y=0
    for x in $(seq 100 5000 100000); do
        y=$(( y + 1 ))
        ./send localhost:$port "$testdir"/in/in_$y.dat >> "$testdir"/secrets.txt &
        sleep 1
    done
    #wait for secrets to be available
    while [[ $(wc -l "$testdir"/secrets.txt | cut -d" " -f1) -lt 20 ]]; do
        sleep 1
    done
    #run all the receives
    y=0
    for secret in $(cat "$testdir"/secrets.txt); do
        y=$(( y + 1 ))
        ./receive localhost:$port $secret "$testdir"/out/out_$y.dat &
    done

    while true; do
        sends=$(pgrep send | wc -l)
        recvs=$(pgrep receive | wc -l)
        if [[ $sends -eq 0 && $recvs -eq 0 ]]; then
            break
        fi
        echo "Waiting for sends/receives"
        sleep 1
    done

    #verify the results
    y=0
    for x in $(seq 100 5000 100000); do
        y=$(( y + 1 ))
        insum=$(md5sum "$testdir"/in/in_$y.dat | awk '{print $1}')
        outsum=$(md5sum "$testdir"/out/out_$y.dat | awk '{print $1}')
        if [[ "$insum" != "$outsum" ]]; then
            echo -e "${red}Copy failed: $y${reset}"
            ret=1
        fi
    done
    #exit $ret
}

run_tests
if [[ $? -gt 0 ]]; then
    cleanup_failure
else
    cleanup_success
fi
