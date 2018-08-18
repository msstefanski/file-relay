#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <unistd.h>
#include <libgen.h>
#include <openssl/sha.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <arpa/inet.h>

#include "secret.h"

static const uint32_t identity = 0xadeafbee;
static const uint32_t relayid  = 0xdeadbeef;


void help()
{
    printf("usage: ./send <relay-host>:<relay-port> <file-to-send>\n");
}

int main(int argc, char *argv[0])
{
    //read host and port from args. would normally use getopt here, but this is
    //a simple program with no configurable options
    if (argc != 3) {
        help();
        exit(1);
    }
    char *address = argv[1];
    char *filename = strdup(argv[2]);
    char *host = strtok(address, ":");
    char *portstr = strtok(NULL, ":");
    if (!host || !portstr) {
        fprintf(stderr, "Invalid host or port\n");
        exit(1);
    }
    int port = strtol(portstr, NULL, 10);

    //Stat the file to ensure it exists and is readable before bothering with
    //anything else
    struct stat file_info;
    if (stat(filename, &file_info)) {
        fprintf(stderr, "Failed to stat file\n");
        exit(1);
    }

    //Generate a secret code and print it. Note: This is the only output on stdout!
    char *secret = make_secret(4);
    printf("%s\n", secret);
    fflush(stdout);

    //Hash the secret so we never transmit the secret itself
    char *hash = make_hash(secret);
    //fprintf(stderr, "%s\n", hash);

    //Get the IP of the host if a hostname was provided
    struct hostent *he;
    he = gethostbyname(host);
    if (!he) {
        fprintf(stderr, "Failed getting IP of %s\n", host);
        exit(1);
    }
    struct in_addr **addr_list;
    addr_list = (struct in_addr **) he->h_addr_list;
    in_addr_t host_ip = inet_addr(inet_ntoa(*addr_list[0]));

    //Create the network socket and connect to host and port
    int sd = socket(AF_INET, SOCK_STREAM, 0);
    if (sd < 0) {
        fprintf(stderr, "Failed to create socket: %s\n", strerror(errno));
        exit(1);
    }
    struct sockaddr_in addr;
    addr.sin_addr.s_addr = host_ip;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (connect(sd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Failed to connect to server\n");
        exit(1);
    }

    //Let server identify itself
    uint32_t response;
    recv(sd, &response, 4, 0);
    if (response != relayid) {
        fprintf(stderr, "Server didn't respond correctly\n");
        close(sd);
        exit(1);
    }

    //Identify us to the relay server as a sender
    send(sd, &identity, 4, 0);

    //Send the secret code hash to pair us with a receiver
    send(sd, hash, SHA_DIGEST_LENGTH*2, 0);

    //Send the filename
    char *base = basename(filename);
    uint16_t fsize = htons(strlen(base) + 1);
    send(sd, &fsize, 2, 0);
    send(sd, base, strlen(base) + 1, 0);

    //Read file, encrypt data using secret, send encrypted data to socket
    char cpbuf[8192];
    int fd = open(filename, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "Failed to open %s: %s\n", filename, strerror(errno));
        goto cleanup_exit;
    }

    while (1) {
        ssize_t rres = read(fd, &cpbuf[0], 8192);
        if (rres < 0) {
            if (errno == EAGAIN) {
                fprintf(stderr, "EAGAIN\n");
                continue;
            } else {
                fprintf(stderr, "fail: %s\n", strerror(errno));
                break;
            }
        } else if (rres == 0) {
            break;
        }
        ssize_t wres = write(sd, &cpbuf[0], rres);
        if (wres != rres) {
            fprintf(stderr, "failed to copy data\n");
            break;
        }
    }

cleanup_exit:
    close(fd);
    close(sd);

    free(filename);
    free(secret);
    free(hash);
}
