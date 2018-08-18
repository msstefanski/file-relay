#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <unistd.h>
#include <linux/limits.h>
#include <openssl/sha.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include "secret.h"

static const uint32_t identity = 0xfacadeed;
static const uint32_t relayid  = 0xdeadbeef;


void help()
{
    printf("usage: ./receive <relay-host>:<relay-port> <secret-code> <output-directory\n");
}

int main(int argc, char *argv[0])
{
    //read host, port, secret, and output location from args
    if (argc != 4) {
        help();
        exit(1);
    }
    char *address = argv[1];
    char *secret = argv[2];
    char *outdir = argv[3];
    char *host = strtok(address, ":");
    char *portstr = strtok(NULL, ":");
    if (!host || !portstr) {
        fprintf(stderr, "Invalid host or port\n");
        exit(1);
    }
    int port = strtol(portstr, NULL, 10);

    //sha1 hash the secret
    char *hash = make_hash(secret);

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
    if (send(sd, &identity, 4, 0) != 4) {
        fprintf(stderr, "Failed to send identity to relay\n");
        close(sd);
        exit(1);
    }

    //Send the secret code hash to pair us with a receiver
    if (send(sd, hash, SHA_DIGEST_LENGTH*2, 0) != SHA_DIGEST_LENGTH*2) {
        fprintf(stderr, "Failed to send hash to relay\n");
        close(sd);
        exit(1);
    }

    //Receive the filename from the server
    char filename[PATH_MAX];
    uint16_t fsize = 0;
    recv(sd, &fsize, 2, 0);
    fsize = ntohs(fsize);
    ssize_t len = recv(sd, filename, fsize, 0);
    if (len == 0) {
        fprintf(stderr, "Read 0 from relay...\n");
        goto cleanup_exit;
    } else if (len != fsize) {
        fprintf(stderr, "Failed to read filename from relay\n");
        goto cleanup_exit;
    }
    filename[len] = '\0';
    char fullfile[PATH_MAX];
    snprintf(fullfile, PATH_MAX, "%s/%s", outdir, filename);

    //Set socket to non-blocking
    //int flags = fcntl(sd, F_GETFL, 0);
    //fcntl(sd, F_GETFL, flags & ~O_NONBLOCK);

    //Open the output file to write to
    int fd = open(fullfile, O_CREAT | O_WRONLY, 0644);
    if (fd < 0) {
        fprintf(stderr, "Failed to open %s: %s\n", fullfile, strerror(errno));
        goto cleanup_exit;
    }

    //recv data from socket
    char cpbuf[8192];
    while (1) {
        ssize_t rres = recv(sd, cpbuf, 8192, 0);
        if (rres < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            } else {
                fprintf(stderr, "Fail: %s\n", strerror(errno));
                break;
            }
        } else if (rres == 0) {
            break;
        }
        ssize_t wres = write(fd, cpbuf, rres);
        if (wres == 0) {
            fprintf(stderr, "Failed to write data\n");
            break;
        } else if (wres != rres) {
            fprintf(stderr, "Failed to copy data\n");
            break;
        }
    }

    close(fd);
cleanup_exit:
    close(sd);

    free(hash);
}
