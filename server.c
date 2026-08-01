#define _POSIX_C_SOURCE 200809L
#include "dict.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
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
 *   - Commands: PING, ECHO, SET, GET, DEL, EXISTS, EXPIRE, TTL, PEXPIREAT
 *   - Key expiry: passive (on access) + active (periodic background sweep)
 *   - AOF persistence: writes are logged to disk and replayed on startup
 *
 * WHAT'S NEXT (Week 4 polish):
 *   - LRU eviction, benchmarking, tests
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
#define MAX_FDS 1024            /* we track each connection in a slot indexed by its fd */
#define ACTIVE_EXPIRE_MS 100   /* how often the background sweep runs (milliseconds) */
#define AOFPATH "miniredis.aof" /* the append-only file: our on-disk write log */

static dict *g_store;         /* the global keyspace */
static FILE *g_aof = NULL;    /* the append-only file (NULL while replaying / if disabled) */

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

/* Milliseconds since the epoch — used for TTLs / key expiry. */
static long long now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000; // converting to milliseconds
}

/* Append a command line to the AOF (our on-disk write log). It's a no-op while
 * g_aof is NULL — which is the case during startup replay, so replayed commands
 * don't get logged a second time. */
static void aof_append(const char *fmt, ...) {
    if (!g_aof) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_aof, fmt, ap);
    va_end(ap);
    fflush(g_aof); /* push out of the C-library buffer so a restart won't lose it.
                      For crash durability you'd also fsync() here — that's Redis's
                      appendfsync knob (default: once per second). */
}

/* Send a C string to the client. fd < 0 is the replay sentinel: no socket to write to. */
static void reply(int fd, const char *s) {
    if (fd < 0) return;
    write(fd, s, strlen(s));
}

/* ---------------------------------------------------------------------------
 * Expiry helpers.
 * ------------------------------------------------------------------------- */

/* Has this entry's TTL already passed? (expireAtMs == -1 means "never expires") */
static int expired(dictEntry *e) {
    return e->expireAtMs != -1 && now_ms() >= e->expireAtMs;
}

/* Look up a key, transparently deleting it if its TTL has passed. This is
 * PASSIVE (lazy) expiration: an expired key can sit in memory until someone
 * touches it, at which point we drop it. Returns the live entry, or NULL. */
static dictEntry *lookup(const char *key) {
    dictEntry *e = dictFind(g_store, key);
    if (e && expired(e)) { dictDel(g_store, key); return NULL; }
    return e;
}

/* ---------------------------------------------------------------------------
 * Command handlers.  Each gets the already-tokenized argv (argv[0] = command).
 * Write commands also append themselves to the AOF.
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
    aof_append("SET %s %s\n", argv[1], argv[2]);   /* persist the write */
    reply(fd, "OK\n"); /* dictSet also clears any prior TTL, like real Redis */
}

static void cmd_get(int fd, int argc, char **argv) {
    if (argc < 2) { reply(fd, "ERR wrong number of arguments for 'get'\n"); return; }
    dictEntry *e = lookup(argv[1]);       /* lazily drops the key if its TTL has passed */
    if (!e) { reply(fd, "(nil)\n"); return; }
    reply(fd, e->val);
    reply(fd, "\n");
}

static void cmd_del(int fd, int argc, char **argv) {
    if (argc < 2) { reply(fd, "ERR wrong number of arguments for 'del'\n"); return; }
    if (dictDel(g_store, argv[1])) {
        aof_append("DEL %s\n", argv[1]);           /* persist the delete */
        reply(fd, "1\n");
    } else {
        reply(fd, "0\n");
    }
}

static void cmd_exists(int fd, int argc, char **argv) {
    if (argc < 2) { reply(fd, "ERR wrong number of arguments for 'exists'\n"); return; }
    if (lookup(argv[1]) != NULL) reply(fd, "1\n"); /* found (and not expired) */
    else                         reply(fd, "0\n"); /* not found */
}

static void cmd_expire(int fd, int argc, char **argv) {
    if (argc < 3) { reply(fd, "ERR wrong number of arguments for 'expire'\n"); return; }
    dictEntry *e = lookup(argv[1]);
    if (!e) { reply(fd, "0\n"); return; }             /* no such key => 0, like Redis */
    long long secs = atoll(argv[2]);
    e->expireAtMs = now_ms() + secs * 1000;           /* stamp the deadline */
    /* Log as an ABSOLUTE deadline so a restart restores the real expiry time
     * instead of resetting the countdown (this is why we have PEXPIREAT). */
    aof_append("PEXPIREAT %s %lld\n", argv[1], e->expireAtMs);
    reply(fd, "1\n");
}

/* PEXPIREAT key ms — set an absolute expiry deadline (ms since the epoch).
 * Mostly an internal/AOF command: EXPIRE is logged as PEXPIREAT so replay
 * restores the true deadline. */
static void cmd_pexpireat(int fd, int argc, char **argv) {
    if (argc < 3) { reply(fd, "ERR wrong number of arguments for 'pexpireat'\n"); return; }
    dictEntry *e = lookup(argv[1]);
    if (!e) { reply(fd, "0\n"); return; }
    e->expireAtMs = atoll(argv[2]);
    aof_append("PEXPIREAT %s %s\n", argv[1], argv[2]);
    reply(fd, "1\n");
}

static void cmd_ttl(int fd, int argc, char **argv) {
    if (argc < 2) { reply(fd, "ERR wrong number of arguments for 'ttl'\n"); return; }
    dictEntry *e = lookup(argv[1]);
    if (!e)                  { reply(fd, "-2\n"); return; }  /* -2: key doesn't exist */
    if (e->expireAtMs == -1) { reply(fd, "-1\n"); return; }  /* -1: exists, but no TTL set */
    long long ms = e->expireAtMs - now_ms();
    long long secs = ms > 0 ? (ms + 999) / 1000 : 0;         /* round up to whole seconds */
    char out[32];
    snprintf(out, sizeof(out), "%lld\n", secs);
    reply(fd, out);
}

/* Dispatch: match argv[0] (case-insensitively) to a handler. */
static void dispatch(int fd, int argc, char **argv) {
    if (argc == 0) return;
    for (char *p = argv[0]; *p; p++) *p = (char)toupper((unsigned char)*p);

    if      (strcmp(argv[0], "PING")      == 0) cmd_ping(fd, argc, argv);
    else if (strcmp(argv[0], "ECHO")      == 0) cmd_echo(fd, argc, argv);
    else if (strcmp(argv[0], "SET")       == 0) cmd_set(fd, argc, argv);
    else if (strcmp(argv[0], "GET")       == 0) cmd_get(fd, argc, argv);
    else if (strcmp(argv[0], "DEL")       == 0) cmd_del(fd, argc, argv);
    else if (strcmp(argv[0], "EXISTS")    == 0) cmd_exists(fd, argc, argv);
    else if (strcmp(argv[0], "EXPIRE")    == 0) cmd_expire(fd, argc, argv);
    else if (strcmp(argv[0], "PEXPIREAT") == 0) cmd_pexpireat(fd, argc, argv);
    else if (strcmp(argv[0], "TTL")       == 0) cmd_ttl(fd, argc, argv);
    else reply(fd, "ERR unknown command\n");
}

/* Split one line into argv[]. Returns argc. */
static int tokenize(char *line, char **argv) {
    int argc = 0;
    char *tok = strtok(line, " \t\r\n");
    while (tok && argc < MAX_ARGS) {
        argv[argc++] = tok;
        tok = strtok(NULL, " \t\r\n");
    }
    return argc;
}

/* Replay a previously-saved AOF: re-run every logged command to rebuild the
 * keyspace. Runs BEFORE g_aof is opened for appending, so replayed commands
 * aren't logged again. fd = -1 makes reply() a no-op. */
static void aof_load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;                        /* no AOF yet -> fresh start */
    char line[LINE_MAX];
    int count = 0;
    while (fgets(line, sizeof(line), f)) {
        char *argv[MAX_ARGS];
        int argc = tokenize(line, argv);   /* strtok also strips the trailing '\n' */
        if (argc > 0) { dispatch(-1, argc, argv); count++; }
    }
    fclose(f);
    printf("AOF: replayed %d commands from %s\n", count, path);
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

    /* 1) Rebuild state from a previous run, THEN 2) open the AOF for appending.
     * Order matters: replay must happen while g_aof is still NULL so replayed
     * commands don't get re-logged. */
    aof_load(AOFPATH);
    g_aof = fopen(AOFPATH, "a");
    if (!g_aof) perror("fopen aof (continuing without persistence)");

    /* --- the usual socket setup --- */
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

    set_nonblocking(lfd);
    int epfd = epoll_create1(0);
    if (epfd < 0) { perror("epoll_create1"); return 1; }

    struct epoll_event ev;
    ev.events  = EPOLLIN;
    ev.data.fd = lfd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &ev);

    printf("miniredis listening on port %d (epoll event loop, many clients)\n", PORT);

    struct epoll_event events[64];
    long long last_sweep = now_ms();
    for (;;) {
        /* Wait for activity, but wake at least every ACTIVE_EXPIRE_MS so the
         * background expiry sweep still runs when there are no clients. */
        int n = epoll_wait(epfd, events, 64, ACTIVE_EXPIRE_MS);
        if (n < 0) { if (errno == EINTR) continue; perror("epoll_wait"); break; }

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;

            if (fd == lfd) {
                int cfd;
                while ((cfd = accept(lfd, NULL, NULL)) >= 0) {
                    if (cfd >= MAX_FDS) { close(cfd); continue; }
                    set_nonblocking(cfd);
                    struct epoll_event cev;
                    cev.events  = EPOLLIN;
                    cev.data.fd = cfd;
                    epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &cev);
                    conn_new(cfd);
                }
            } else {
                struct conn *c = conns[fd];
                if (!c) continue;
                if (conn_read(c) < 0)
                    conn_close(c);
            }
        }

        /* ACTIVE expiration: periodically sweep out keys whose TTL has passed,
         * so memory isn't held by expired keys that nobody ever reads. */
        long long now = now_ms();
        if (now - last_sweep >= ACTIVE_EXPIRE_MS) {
            dictActiveExpire(g_store, now);
            last_sweep = now;
        }
    }

    dictFree(g_store);
    close(lfd);
    return 0;
}
