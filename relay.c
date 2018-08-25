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
#include <linux/limits.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/syscall.h>
#include <openssl/sha.h>

static const uint32_t identity = 0xdeadbeef;
static const uint32_t sender   = 0xadeafbee;
static const uint32_t receiver = 0xfacadeed;
static int lsd = 0; //main socket file descriptor to bind/listen on
static int stop = 0;
SLIST_HEAD(join_head, join_entry) join_head = SLIST_HEAD_INITIALIZER(join_head);
struct join_entry {
    pthread_t thread;
    pid_t tid;
    SLIST_ENTRY(join_entry) entries;
};
static pthread_mutex_t join_lock = PTHREAD_MUTEX_INITIALIZER;
static void *troot = NULL;


void help()
{
    printf("usage: ./relay :<port>\n");
}

void interrupt(int sig)
{
    shutdown(lsd, SHUT_RDWR);
    stop = 1;
}

struct transfer_info {
    char *hash;
    char *filename;
    uint16_t fnlen;
    int infd;
    int outfd;
    pthread_t tid;
    pthread_attr_t tattr;
};

static void transfer_info_free(struct transfer_info *tr)
{
    if (!tr)
        return;
    if (tr->hash)
        free(tr->hash);
    if (tr->filename)
        free(tr->filename);
}

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
        transfer_info_free(t);
        free(t);
        break;
    }
}

static void join_finished_threads()
{
    pthread_mutex_lock(&join_lock);
    struct join_entry *je;
    while (!SLIST_EMPTY(&join_head)) {
        je = SLIST_FIRST(&join_head);
        SLIST_REMOVE_HEAD(&join_head, entries);
        printf("joining finished thread %d\n", je->tid);
        fflush(stdout);
        pthread_join(je->thread, NULL);
        free(je);
    }
    pthread_mutex_unlock(&join_lock);
}

static int copy_using_splice(int in, int out)
{
    int p[2];
    if (pipe(p) < 0) {
        perror("Failed to create pipe!");
        return -1;
    }
    ssize_t s = 0;
    do {
        s = splice(p[0],
                   0,
                   out,
                   0,
                   splice(in, 0, p[1], 0, 8192, SPLICE_F_MORE),
                   SPLICE_F_MORE);
        if (s < 0) {
            perror("Splice failed");
        }
    } while (s > 0 && !stop);
    close(p[0]);
    close(p[1]);
}

static size_t copy_using_read_write_loop(int in, int out)
{
    size_t bytes_copied = 0;
    char cpbuf[8192];
    while (!stop) {
        ssize_t rres = read(in, &cpbuf[0], 8192);
        if (!rres)
            break;
        ssize_t wres = write(out, &cpbuf[0], rres);
        if (wres != rres) {
            fprintf(stderr, "Failed to copy data\n");
            break;
        }
        bytes_copied += wres;
    }
    return bytes_copied;
}

void *relay_data(void *opaque)
{
    struct transfer_info *pair = (struct transfer_info *)opaque;

    if (!pair)
        return NULL;

    if (pair->infd < 0 || pair->outfd < 0 || !pair->hash || !pair->filename) {
        fprintf(stderr, "Transfer info invalid\n");
        goto cleanup;
    }

    pid_t tid = syscall(SYS_gettid);
    printf("thread %d started\n", tid);

    uint16_t fsize = htons(pair->fnlen);
    send(pair->outfd, &fsize, 2, MSG_NOSIGNAL);
    send(pair->outfd, pair->filename, pair->fnlen, MSG_NOSIGNAL);

#ifdef USE_SPLICE
    copy_using_splice(pair->infd, pair->outfd);
#else
    copy_using_read_write_loop(pair->infd, pair->outfd);
#endif

cleanup:
    close(pair->infd);
    close(pair->outfd);
    transfer_info_free(pair);
    free(pair);

    //before we exit, join other exited threads to free resources and prevent
    //maxing out system thread count
    join_finished_threads();

    //now add this thread to the list of threads to join and free later
    pthread_mutex_lock(&join_lock);
    struct join_entry *je = malloc(sizeof(struct join_entry));
    je->thread = pthread_self();
    je->tid = tid;
    SLIST_INSERT_HEAD(&join_head, je, entries);
    pthread_mutex_unlock(&join_lock);

    printf("thread %d exiting\n", tid);

    return NULL;
}

void handle_client_socket(int csd)
{
    printf("Accepted client on fd %d\n", csd);

    //Read byte identifier from socket
    uint32_t response;
    recv(csd, &response, 4, 0);
    if (response != sender && response != receiver) {
        fprintf(stderr, "Client is not a valid sender or receiver\n");
        close(csd);
        return;
    }

    //Read sha hash
    static char shabuf[SHA_DIGEST_LENGTH*2+1];
    recv(csd, shabuf, SHA_DIGEST_LENGTH*2, 0);
    shabuf[SHA_DIGEST_LENGTH*2+1] = '\0';

    static char filebuf[PATH_MAX];
    static uint16_t fsize = 0;
    if (response == sender) {
        printf("got sender with hash %s\n", shabuf);

        //Read incoming filename
        recv(csd, &fsize, 2, 0);
        fsize = ntohs(fsize);
        ssize_t len = recv(csd, filebuf, fsize, 0);
        if (len != fsize) {
            fprintf(stderr, "Failed to read filename from sender\n");
            close(csd);
            return;
        }
        filebuf[len] = '\0';
    }
    else if (response == receiver) {
        printf("got receiver with hash %s\n", shabuf);
    }

    //Set the client socket to allow blocking again (in it's own thread)
    int flags = fcntl(csd, F_GETFL, 0);
    fcntl(csd, F_GETFL, flags | O_NONBLOCK);

    static struct transfer_info tr;
    memset(&tr, 0, sizeof(struct transfer_info));
    tr.hash = shabuf;

    //if got sha hash, check search tree
    void **obj = tfind((void *)&tr, &troot, compare);
    if (obj) {
        //match found, this should be the receiver
        struct transfer_info *match = (struct transfer_info *)*obj;
        if (!tdelete(*obj, &troot, compare)) {
            fprintf(stderr, "No hash found. Failed to delete node\n");
            close(csd);
            return;
        }
        if (match->infd < 0) {
            fprintf(stderr, "Found matching hash but it has a bad fd\n");
            close(csd);
            return;
        }
        match->outfd = csd;
        //spawn new thread
        pthread_attr_init(&match->tattr);
        pthread_attr_setstacksize(&match->tattr, 2048);
        pthread_create(&match->tid, &match->tattr, relay_data, (void *)match);
        //set the thread name so we can identify it easier
        static char thread_name[16];
        snprintf(thread_name, 16, "tfd-%d:%d", match->infd, match->outfd);
        thread_name[15] = '\0';
        pthread_setname_np(match->tid, thread_name);
    } else {
        //not found, this should be the sender
        struct transfer_info *ntr = calloc(1, sizeof(struct transfer_info));
        ntr->hash = strdup(shabuf);
        ntr->filename = strdup(filebuf);
        ntr->fnlen = fsize;
        ntr->infd = csd;
        ntr->outfd = -1;
        obj = tsearch((void *)ntr, &troot, compare);
        if (!obj) {
            fprintf(stderr, "Insufficient memory to add hash to binary search tree\n");
            transfer_info_free(ntr);
            free(ntr);
            close(csd);
            return;
        }
    }
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
    lsd = socket(AF_INET, SOCK_STREAM, 0);
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
        close(lsd);
        exit(1);
    }

    //Accept and handle client connections
    struct epoll_event ev, events[MAX_CONNECTIONS];
    int epollfd = epoll_create1(0);
    if (epollfd < 0) {
        fprintf(stderr, "Failed to create epollfd (%s)\n", strerror(errno));
        close(lsd);
        exit(1);
    }
    ev.events = EPOLLIN;
    ev.data.fd = lsd;
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, lsd, &ev) < 0) {
        fprintf(stderr, "Failed epoll_ctl (%s)\n", strerror(errno));
        close(lsd);
        close(epollfd);
        exit(1);
    }

    int nfds;
    while (!stop) {
        nfds = epoll_wait(epollfd, events, MAX_CONNECTIONS, 100);
        if (nfds < 0) {
            if (nfds == EINTR)
                continue;
            fprintf(stderr, "Failed epoll_wait (%s)\n", strerror(errno));
            break;
        }
        if (nfds > 1)
            printf("Got %d nfds\n", nfds);
        for (int n = 0; n < nfds; n++) {
            if (events[n].data.fd == lsd) {
                socklen_t addr_len = sizeof(addr);
                int csd = accept(lsd, (struct sockaddr *)&addr, &addr_len);
                if (csd < 0) {
                    fprintf(stderr, "Failed to accept client socket: %s\n", strerror(errno));
                    continue;
                }

                //Make our client socket non blocking for the initial send. We
                //don't ever want to block the listen thread, so if a client
                //connects and doesn't respond correctly right away just drop
                //it and wait for a new one.
                int flags = fcntl(csd, F_GETFL, 0);
                fcntl(csd, F_GETFL, flags & ~O_NONBLOCK);

                //Send our identity first
                ssize_t s = send(csd, &identity, 4, MSG_NOSIGNAL);
                if (s < 0) {
                    fprintf(stderr, "Failed to send identity: (%s)\n", strerror(errno));
                    continue;
                } else if (s < 4) {
                    fprintf(stderr, "Failed to send identity\n");
                    continue;
                }

                ev.events = EPOLLIN | EPOLLONESHOT;
                ev.data.fd = csd;
                if (epoll_ctl(epollfd, EPOLL_CTL_ADD, csd, &ev) < 0) {
                    fprintf(stderr, "Failed epoll_ctl on client socket (%s)\n", strerror(errno));
                    exit(1);
                }
            } else {
                printf("Socket %d got activity\n", events[n].data.fd);
                handle_client_socket(events[n].data.fd);
            }
        }
    }

    //twalk to iterate over any umatched connections and close them
    twalk(troot, close_unmatched_connections);
    tdestroy(troot, free);

    join_finished_threads();

    close(epollfd);
    shutdown(lsd, SHUT_RDWR);
    close(lsd);
}
