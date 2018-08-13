#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <search.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <openssl/sha.h>

static const uint32_t identity = 0xdeadbeef;
static const uint32_t sender   = 0xadeafbee;
static const uint32_t receiver = 0xfacadeed;
static int stop = 0;


void help()
{
    printf("usage: ./relay :<port>\n");
}

void interrupt(int sig)
{
    printf("signal received\n");
    stop = 1;
}

struct transfer_info {
    char *hash;
    int infd;
    int outfd;
    pthread_t tid;
    pthread_attr_t tattr;
};

static int compare(const void *a, const void *b)
{
    struct transfer_info *ta = (struct transfer_info *)a;
    struct transfer_info *tb = (struct transfer_info *)b;

    return strncmp(ta->hash, tb->hash, SHA_DIGEST_LENGTH);
}

static void close_unmatched_connections(const void *nodep, const VISIT which, const int depth)
{
    struct transfer_info *t;

    switch(which) {
    case preorder:
        break;
    case endorder:
        break;
    case postorder:
    case leaf:
        t = (struct transfer_info *)nodep;
        close(t->infd);
        close(t->outfd);
        if (t->hash)
            free(t->hash);
        break;
    }
}

static int copy_using_splice(int in, int out)
{
    int p[2];
    pipe(p);
    while (splice(p[0], 0, out, 0, splice(in, 0, p[1], 0, 8192, 0), 0) > 0);
}

static size_t copy_using_read_write_loop(int in, int out)
{
    size_t bytes_copied = 0;
    char cpbuf[8192];
    while (1) {
        ssize_t rres = read(in, &cpbuf[0], 8192);
        if (!rres)
            break;
        ssize_t wres = write(out, &cpbuf[0], rres);
        if (wres != rres) {
            fprintf(stderr, "failed to copy data\n");
            break;
        }
        bytes_copied += wres;
    }
    return bytes_copied;
}

void *relay_data(void *opaque)
{
    struct transfer_info *pair = (struct transfer_info *)opaque;

    if (pair->infd < 0 || pair->outfd < 0 || !pair->hash) {
        fprintf(stderr, "Transfer info invalid\n");
        return NULL;
    }

    printf("thread started\n");
#if 0
    copy_using_splice(pair->infd, pair->outfd);
#else
    size_t b = copy_using_read_write_loop(pair->infd, pair->outfd);
    printf("copied %zu bytes\n", b);
#endif

    close(pair->infd);
    close(pair->outfd);
    free(pair->hash);
    free(pair);

    printf("thread exiting\n");

    return NULL;
}

int main(int argc, char *argv[])
{
    signal(SIGINT, interrupt);
    signal(SIGTERM, interrupt);

    //read port from args
    if (argc != 2) {
        help();
        exit(1);
    }
    char *address = argv[1];
    char *portstr = strtok(address, ":");
    if (!portstr) {
        portstr = address;
    } else {
        address = strtok(NULL, ":");
        if (address)
            portstr = address;
    }
    if (!portstr) {
        fprintf(stderr, "Invalid input\n");
        exit(1);
    }
    int port = strtol(portstr, NULL, 10);

    //Create listen socket and bind to it
    int lsd = socket(AF_INET, SOCK_STREAM, 0);
    if (lsd < 0) {
        fprintf(stderr, "Failed to create socket: %s\n", strerror(errno));
        exit(1);
    }
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (bind(lsd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Failed to bind to socket: %s\n", strerror(errno));
        exit(1);
    }
    if (listen(lsd, MAX_CONNECTIONS) < 0) {
        fprintf(stderr, "Failed to listen on socket: %s\n", strerror(errno));
        exit(1);
    }

    //Accept and handle client connections
    fd_set rdfs;
    struct timeval tv;
    void *troot = NULL;
    struct transfer_info tr;
    while (!stop) {
        FD_ZERO(&rdfs);
        FD_SET(lsd, &rdfs);
        tv.tv_sec = 0;
        tv.tv_usec = 250000;
        int sig = select(lsd+1, &rdfs, NULL, NULL, &tv);
        if (sig < 0) {
            fprintf(stderr, "%s\n", strerror(errno));
            break;
        } else if (sig == 0) {
            //timeout reached, allows us to exit on demand
            continue;
        }

        socklen_t addr_len = sizeof(addr);
        int csd = accept(lsd, (struct sockaddr *)&addr, &addr_len);
        if (csd < 0) {
            fprintf(stderr, "Failed to accept client socket: %s\n", strerror(errno));
            continue;
        }
        //make our client socket non blocking for the initial send
        //int flags = fcntl(csd, F_GETFL, 0);
        //fcntl(csd, F_GETFL, flags & ~O_NONBLOCK);

        //Send our identity first
        send(csd, &identity, 4, 0);

        //read byte identifier from socket
        uint32_t response;
        recv(csd, &response, 4, 0);
        if (response != sender && response != receiver) {
            fprintf(stderr, "Client is not a valid sender or receiver\n");
            close(csd);
            continue;
        }

        //read sha hash
        char buffer[SHA_DIGEST_LENGTH*2+1];
        recv(csd, buffer, SHA_DIGEST_LENGTH*2, 0);
        buffer[SHA_DIGEST_LENGTH*2+1] = '\0';

        //allow blocking again
        //fcntl(csd, F_GETFL, flags | O_NONBLOCK);

        if (response == sender)
            printf("got sender with hash %s\n", buffer);
        else if (response == receiver)
            printf("got receiver with hash %s\n", buffer);

        memset(&tr, 0, sizeof(struct transfer_info));
        tr.hash = buffer;
        tr.infd = csd;

        //if got sha hash, check search tree
        void **obj = tfind((void *)&tr, &troot, compare);
        if (obj) {
            //match found, this should be the receiver
            struct transfer_info *match = (struct transfer_info *)*obj;
            if (!tdelete(*obj, &troot, compare)) {
                fprintf(stderr, "No hash found. Failed to delete node\n");
                continue;
            }
            //TODO: move this to a thread list so we can join them on stop and free memory afterwards
            if (match->infd < 0) {
                fprintf(stderr, "Found matching hash but it has a bad fd\n");
                close(csd);
                continue;
            }
            match->outfd = csd;
            //spawn new thread
            pthread_attr_init(&match->tattr);
            //pthread_attr_setstacksize(&match->tattr, 12048);
            pthread_create(&match->tid, &match->tattr, relay_data, (void *)match);
        } else {
            //not found, this should be the sender
            struct transfer_info *ntr = calloc(1, sizeof(struct transfer_info));
            ntr->hash = strdup(buffer);
            ntr->infd = csd;
            obj = tsearch((void *)ntr, &troot, compare);
            if (!obj) {
                fprintf(stderr, "Insufficient memory to add hash to binary search tree\n");
                close(csd);
                continue;
            }
        }
    }

    //twalk to iterate over any umatched connections and close them
    twalk(troot, close_unmatched_connections);
    tdestroy(troot, free);

    close(lsd);
}
