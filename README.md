# Description

# Usage

# Design Choices

## Golang vs C
Go is a great choice for this type of problem because of language focus on
networking and concurrency. Goroutines are great for high concurrency, and
multiple goroutines can run on a single OS thread, but not for blocking I/O.
When the goroutine is blocked on I/O, Go shifts the goroutine to a new OS
thread. Since each send/receive connection will be blocking on socket I/O it's
entire existance, goroutines will mostly map one to one to OS threads anyway,
and the advantages of (lightweight) goroutines largly disappears.

Goroutines default to 2KB stack that grows as needed at the expense of function
call overhead. Each function call a check is done if there's sufficient stack
for the function to run. If not, a new larger stack is allocated on the heap
and data is copied to the new stack, the old stack is freed, and the original
function call is restarted.

In C, the default stack size of a new thread is often defaulted to 8MB! This
can be controlled with ulimit, or by manually setting the stack size on thread
creation. We know exactly how much stack is needed for a thread, so we can set
it to a minimum value we've predetermined based on knowledge of how this
architecture should work.

To maximize performance we often need to leverage OS-dependent kernel features.
The Go compiler is on all major operating systems, allowing OS-agnostic tools
to be built. We then rely on Go to leveraging those kernel features for
performance for us, and it may not make the same choices we would make
ourselves.

## C Design
* int pthread_attr_setstacksize(pthread_attr_t *attr, size_t stacksize);
  - set stack size to minimal amount needed for an 8KB buffer for splice, plus
    socket, etc. Since we know exactly what each thread is doing we can limit
    the stack size allowing for more total concurrent threads on the system
  - How small can we set the stack size? As far below 4MB as possible :)
* zero-copy between sockets: use splice function to do copies between sockets
  without having to copy data to userspace first.
* hash map to store the hashed secret sent by `send` tool, once a matching
  hashed secret received by `receive` tool is met, spawn a new thread to handle
  splice between the two sockets, then shut down both sockets and the thread
  - use hcreate_r and related functions? these are not POSIX, but standard
    hcreate only allows a single hash map which is really all we need anyway
  - splice is linux-only. used a read/write loop as backup for OSX, etc.
  - while (splice(... SPLICE_F_MORE | SPLICE_F_MOVE) > 0);
* authentication of `send` and `receive` with `reply`: OAuth token, SHA hash
  based on some known value, or UUID?
  - use a simple predefined byte sequence for identification: e.g. `send`
    identifies with 0xadeafbee, `receive` identifies with 0xbefacade and
    `relay` replies to both with 0xdeadbeef
* secret sharing
  - `send` creates a secret (random collection of dictionary words) and prints it
  - `send` and `receive` get sha1 of the secret to send to `relay`
  - read /usr/share/dict/american for random words, separate words by random
    special characters or numbers
* SSL sockets?? this would probably increase stack size requirement and how
  effectively splice can be used to copy between sockets?
  - No! Encrypt in `send` based on the secret and decrypt in `receive` using
    the same secret. A simple byte modifying algorithm can be used as a
    stand-in here. This keeps `relay` small and fast and also never receives
    unencrypted data or has the ability to decrypt it since it never knows the
    actual secret, only a hash of that secret.

Sample 4 byte identification sequences:
0xdeadbeef
0xadeafbee
0xbefacade

### Questions
Do we need a dedicated thread to receive the secret (and possibly
authentication token) from each connection so we don't block the listen thread?

### Libraries
uuid/uuid.h
openssl/sha.h
