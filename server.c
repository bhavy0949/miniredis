#define _POSIX_C_SOURCE 200809L
#include "dict.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>   /* epoll — Linux's I/O event notification (build/run on Linux, e.g. Docker) */
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/*
 * miniredis — a tiny in-memory key-value server.
 *
 * WHAT WORKS RIGHT NOW:
 *   - An EVENT-DRIVEN TCP server on port 6379 (single thread, many clients)
 *     using epoll for I/O multiplexing.
 *   - An inline text protocol (one command per line, space-separated)
 *   - Commands: PING, ECHO, SET, GET, DEL, EXISTS
 *
 * WHAT'S NEXT:
 *   - EXPIRE / TTL ...... Day 7-8 (the expireAtMs field in dict.h is waiting)
 *   - AOF persistence ... Day 9-10
 *
 * Platform note: epoll is Linux-only. On macOS, build & run inside the
 * provided Docker container (see the Dockerfile / README).
 *
 * Test it with:   nc 127.0.0.1 6379     then type:  PING
 */

#define PORT 6379
#define BACKLOG 16
#define MAX_ARGS 8
#define LINE_MAX 4096
#define MAX_FDS 1024     /* we track each connection in a slot indexed by its fd */

static dict *g_store; /* the global keyspace */

/* ---------------------------------------------------------------------------
 * Per-connection state: each client gets its OWN "notepad" (read buffer),
 * because with many clients at once their data arrives interleaved.
 * ------------------------------------------------------------------------- */
struct conn {
    int    fd;              /* this client's socket */
    char   buf[LINE_MAX];   /* bytes received but not yet fully processed */
    size_t used;            /* how many bytes are currently in buf */
};

/* conns[fd] points to that fd's state, or is NULL if the slot is free. */
static struct conn *conns[MAX_FDS];

/* Milliseconds since the epoch — you'll need this for TTLs on Day 7. */
__attribute__((unused))
static long long now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* Send a C string to the client. */
static void reply(int fd, const char *s) {
    write(fd, s, strlen(s));
}

/* ---------------------------------------------------------------------------
 * Command handlers.  Each gets the already-tokenized argv (argv[0] = command).
 * (UNCHANGED — the event loop doesn't touch command logic.)
 * ------------------------------------------------------------------------- */

static void cmd_ping(int fd, int argc, char **argv) {
    (void)argc; (void)argv;
    reply(fd, "PONG\n");
}

static void cmd_echo(int fd, int argc, char **argv) {
    if (argc < 2) { reply(fd, "ERR wrong number of arguments for 'echo'\n"); return; }
    reply(fd, argv[1]);
    reply(fd, "\n");
}

static void cmd_set(int fd, int argc, char **argv) {
    if (argc < 3) { reply(fd, "ERR wrong number of arguments for 'set'\n"); return; }
    if (dictSet(g_store, argv[1], argv[2]) != 0) { reply(fd, "ERR out of memory\n"); return; }
    reply(fd, "OK\n");
}

static void cmd_get(int fd, int argc, char **argv) {
    if (argc < 2) { reply(fd, "ERR wrong number of arguments for 'get'\n"); return; }
    dictEntry *e = dictFind(g_store, argv[1]);
    /* Day 7: before returning the value, check e->expireAtMs against now_ms()
     * and treat the key as absent (and delete it) if it has expired. */
    if (!e) { reply(fd, "(nil)\n"); return; }
    reply(fd, e->val);
    reply(fd, "\n");
}

static void cmd_del(int fd, int argc, char **argv) {
    if (argc < 2) { reply(fd, "ERR wrong number of arguments for 'del'\n"); return; }
    if (dictDel(g_store, argv[1])) reply(fd, "1\n"); /* dictDel returns 1 if it removed a key */
    else                           reply(fd, "0\n"); /* 0 if the key wasn't there */
}

static void cmd_exists(int fd, int argc, char **argv) {
    if (argc < 2) { reply(fd, "ERR wrong number of arguments for 'exists'\n"); return; }
    if (dictFind(g_store, argv[1]) != NULL) reply(fd, "1\n"); /* found it */
    else                                    reply(fd, "0\n"); /* not found */
}

/* Dispatch: match argv[0] (case-insensitively) to a handler. (UNCHANGED.) */
static void dispatch(int fd, int argc, char **argv) {
    if (argc == 0) return;
    for (char *p = argv[0]; *p; p++) *p = (char)toupper((unsigned char)*p);

    if      (strcmp(argv[0], "PING")   == 0) cmd_ping(fd, argc, argv);
    else if (strcmp(argv[0], "ECHO")   == 0) cmd_echo(fd, argc, argv);
    else if (strcmp(argv[0], "SET")    == 0) cmd_set(fd, argc, argv);
    else if (strcmp(argv[0], "GET")    == 0) cmd_get(fd, argc, argv);
    else if (strcmp(argv[0], "DEL")    == 0) cmd_del(fd, argc, argv);
    else if (strcmp(argv[0], "EXISTS") == 0) cmd_exists(fd, argc, argv);
    else reply(fd, "ERR unknown command\n");
}

/* Split one line into argv[]. Returns argc. (UNCHANGED.) */
static int tokenize(char *line, char **argv) {
    int argc = 0;
    char *tok = strtok(line, " \t\r\n");
    while (tok && argc < MAX_ARGS) {
        argv[argc++] = tok;
        tok = strtok(NULL, " \t\r\n");
    }
    return argc;
}

/* ---------------------------------------------------------------------------
 * Event-loop plumbing.
 * ------------------------------------------------------------------------- */

/* Flip a socket into "non-blocking" mode: reads/accepts never freeze; if there's
 * nothing ready they return immediately with errno == EAGAIN. */
static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* Allocate state for a newly-accepted connection. */
static struct conn *conn_new(int fd) {
    struct conn *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->fd = fd;
    c->used = 0;
    conns[fd] = c;
    return c;
}

/* Tear a connection down. Closing the socket auto-removes it from epoll. */
static void conn_close(struct conn *c) {
    close(c->fd);
    conns[c->fd] = NULL;
    free(c);
}

/* A client's socket is readable: drain what's available into its buffer, then
 * run every complete (newline-terminated) command. Returns -1 if the connection
 * should be closed (client hung up or errored), 0 to keep it open. */
static int conn_read(struct conn *c) {
    ssize_t n = read(c->fd, c->buf + c->used, sizeof(c->buf) - c->used - 1);
    if (n == 0) return -1;                                  /* client closed */
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0; /* nothing right now */
        return -1;                                          /* a real error */
    }

    c->used += (size_t)n;
    c->buf[c->used] = '\0';

    char *start = c->buf, *nl;
    while ((nl = strchr(start, '\n')) != NULL) {
        *nl = '\0';
        char *argv[MAX_ARGS];
        char linecopy[LINE_MAX];
        snprintf(linecopy, sizeof(linecopy), "%s", start);
        int argc = tokenize(linecopy, argv);
        dispatch(c->fd, argc, argv);
        start = nl + 1;
    }
    c->used = strlen(start);
    memmove(c->buf, start, c->used + 1);
    return 0;
}

int main(void) {
    g_store = dictCreate(1024);
    if (!g_store) { perror("dictCreate"); return 1; }

    /* Day 9-10: replay the AOF file here so we come back up with prior data. */

    /* --- the usual socket setup (UNCHANGED) --- */
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) { perror("socket"); return 1; }

    int yes = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(PORT);

    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(lfd, BACKLOG) < 0) { perror("listen"); return 1; }

    /* --- THE EPOLL EVENT LOOP --- */

    set_nonblocking(lfd);                 /* the listener never blocks either */

    int epfd = epoll_create1(0);          /* create the epoll instance ("notification service") */
    if (epfd < 0) { perror("epoll_create1"); return 1; }

    /* Tell epoll to watch the listener for "someone is trying to connect". */
    struct epoll_event ev;
    ev.events  = EPOLLIN;                 /* EPOLLIN = "there is data to read / a client to accept" */
    ev.data.fd = lfd;                     /* remember which fd this event is about */
    epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &ev);

    printf("miniredis listening on port %d (epoll event loop, many clients)\n", PORT);

    struct epoll_event events[64];        /* epoll hands us up-to-64 ready fds per wakeup */
    for (;;) {
        int n = epoll_wait(epfd, events, 64, -1);   /* sleeps until something is ready (-1 = forever) */
        if (n < 0) { if (errno == EINTR) continue; perror("epoll_wait"); break; }

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;             /* which socket is ready */

            if (fd == lfd) {
                /* The listener is ready => one or more new clients are waiting.
                 * Accept them all (loop until accept says "no more"). */
                int cfd;
                while ((cfd = accept(lfd, NULL, NULL)) >= 0) {
                    if (cfd >= MAX_FDS) { close(cfd); continue; }   /* too many; drop it */
                    set_nonblocking(cfd);
                    struct epoll_event cev;
                    cev.events  = EPOLLIN;
                    cev.data.fd = cfd;
                    epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &cev);      /* watch this client too */
                    conn_new(cfd);                                  /* give it a notepad */
                }
                /* accept() returning -1 with EAGAIN just means "no more waiting". */
            } else {
                /* An existing client sent us something. */
                struct conn *c = conns[fd];
                if (!c) continue;
                if (conn_read(c) < 0)
                    conn_close(c);               /* client left or errored => clean up */
            }
        }
    }

    dictFree(g_store);
    close(lfd);
    return 0;
}
