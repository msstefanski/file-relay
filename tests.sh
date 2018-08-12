#!/bin/bash

# increase soft file descriptor limit
#ulimit -n 10032

if [[ $# -gt 0 ]]; then
    port=$1
else
    port=8082
fi

red='\033[0;31m'
green='\033[0;32m'
reset='\x1b[0m'

function cleanup_failure {
    echo -e "${red}Tests failed!${reset}"
    kill -9 $(pgrep relay)
}

function cleanup_success {
    echo -e "${green}Tests passed!${reset}"
    kill -2 $(pgrep relay)
}

#trap 'cleanup_failure' SIGTERM SIGINT SIGSEGV SIGABRT
#trap 'cleanup_success' EXIT

function run_tests {
    set -e
    mkdir -p testout testin

    ./relay :"$port" &

    #run all the send/receives
    for x in $(seq 100 10000 10000000); do
        head -c $x /dev/urandom | tr -dc 'a-zA-Z0-9~!@#$%^&*_-' > testin/tmp_$x.txt
        secret=$(./send localhost:"$port" testin/tmp_$x.txt)
        ./receive localhost:"$port" "$secret" testout/tmp_$x.txt
    done

    while true; do
        sends=$(pgrep send | wc -l)
        recvs=$(pgrep receive | wc -l)
        if [[ $sends -eq 0 && $recvs -eq 0 ]]; then
            break
        fi
        sleep 1
    done

    #verify the results
    for x in $(seq 100 10000 10000000); do
        insum=$(md5sum testin/tmp_$x.txt | awk '{print $1}')
        outsum=$(md5sum testout/tmp_$x.txt | awk '{print $1}')
        if [[ "$insum" != "$outsum" ]]; then
            echo -e "${red}Copy failed${reset}"
        fi
    done
}

run_tests
if [[ $? -gt 0 ]]; then
    cleanup_failure
else
    cleanup_success
fi
