#define _POSIX_C_SOURCE 200809L
#include "dict.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/*
 * miniredis — a tiny in-memory key-value server.
 *
 * WHAT WORKS RIGHT NOW (your Day 1-3 starting point):
 *   - A blocking TCP server on port 6379
 *   - An inline text protocol (one command per line, space-separated)
 *   - Commands: PING, ECHO, SET, GET, DEL, EXISTS
 *
 * WHAT'S YOURS TO BUILD (this is the resume-defining work):
 *   - Event loop ........ Day 5-6 (see the big block in main() — THE centerpiece)
 *   - EXPIRE / TTL ...... Day 7-8 (the expireAtMs field in dict.h is waiting for you)
 *   - AOF persistence ... Day 9-10
 *
 * Test it with:   nc 127.0.0.1 6379     then type:  PING
 */

#define PORT 6379
#define BACKLOG 16
#define MAX_ARGS 8
#define LINE_MAX 4096

static dict *g_store; /* the global keyspace */

/* Milliseconds since the epoch — you'll need this for TTLs on Day 7.
 * (Marked unused for now so the scaffold compiles warning-free; delete the
 * attribute once you start calling it.) */
__attribute__((unused))
static long long now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* Send a C string to the client. (For the MVP we ignore partial writes;
 * making writes robust under a full socket buffer is a good Day 12 polish task.) */
static void reply(int fd, const char *s) {
    write(fd, s, strlen(s));
}

/* ---------------------------------------------------------------------------
 * Command handlers.  Each gets the already-tokenized argv (argv[0] = command).
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

/* ---------------------------------------------------------------------------
 * Dispatch: match argv[0] (case-insensitively) to a handler.
 * ------------------------------------------------------------------------- */
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

/* Split one line into argv[]. Returns argc. (strtok is fine for the MVP;
 * note it means values can't contain spaces — RESP fixes that later.) */
static int tokenize(char *line, char **argv) {
    int argc = 0;
    char *tok = strtok(line, " \t\r\n");
    while (tok && argc < MAX_ARGS) {
        argv[argc++] = tok;
        tok = strtok(NULL, " \t\r\n");
    }
    return argc;
}

/* Serve one client until it disconnects. BLOCKING: while we're in here, no
 * other client can be served. Day 5-6 is about removing this limitation. */
static void handle_client(int fd) {
    char buf[LINE_MAX];
    size_t used = 0;

    for (;;) {
        ssize_t n = read(fd, buf + used, sizeof(buf) - used - 1);
        if (n <= 0) return; /* client closed or error */
        used += (size_t)n;
        buf[used] = '\0';

        /* process every complete line currently in the buffer */
        char *start = buf, *nl;
        while ((nl = strchr(start, '\n')) != NULL) {
            *nl = '\0';
            char *argv[MAX_ARGS];
            char linecopy[LINE_MAX];
            snprintf(linecopy, sizeof(linecopy), "%s", start);
            int argc = tokenize(linecopy, argv);
            dispatch(fd, argc, argv);
            start = nl + 1;
        }
        /* shift any partial (unterminated) line to the front of the buffer */
        used = strlen(start);
        memmove(buf, start, used + 1);
    }
}

int main(void) {
    g_store = dictCreate(1024);
    if (!g_store) { perror("dictCreate"); return 1; }

    /* Day 9-10: after creating the store, replay the AOF file here so the
     * server comes back up with all previously-written data. */

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
    printf("miniredis listening on port %d (blocking, one client at a time)\n", PORT);

    /* ======================================================================
     * DAY 5-6: THE EVENT LOOP  — this is the heart of the whole project.
     *
     * Right now we accept ONE client and serve it to completion before we
     * even look at the next connection. That's the same limitation your
     * banking server had (it used fork() to work around it).
     *
     * Replace this accept-then-handle_client loop with a SINGLE-THREADED
     * EVENT LOOP:
     *
     *   1. Set the listener socket non-blocking.
     *   2. Create a kqueue  (macOS)  /  epoll  (Linux)  instance.
     *   3. Register the listener for "readable" events.
     *   4. Loop forever on kevent()/epoll_wait():
     *        - if the listener is readable -> accept(), set the new fd
     *          non-blocking, register it too.
     *        - if a client fd is readable  -> read what's available into
     *          THAT client's own buffer, extract complete lines, dispatch.
     *        - on EOF/error -> close and unregister the fd.
     *
     * The key mental shift: each client needs its OWN read buffer (a small
     * struct per fd), because reads now arrive in interleaved fragments.
     * Once this works, one thread serves hundreds of clients concurrently —
     * that's your headline resume bullet.
     * ==================================================================== */
    for (;;) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) { perror("accept"); continue; }
        handle_client(cfd); /* <-- blocking; replace with event-loop dispatch */
        close(cfd);
    }

    dictFree(g_store);
    close(lfd);
    return 0;
}
