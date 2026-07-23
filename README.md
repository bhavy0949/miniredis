# miniredis

A tiny, from-scratch, in-memory key–value server written in C — a learning-grade
clone of the core of [Redis](https://redis.io). Single-threaded, event-driven,
with key expiry and append-only-file persistence.

> **Status: 🚧 in progress.** Built as a systems-programming portfolio project.
> The goal is to understand — and be able to explain — how a high-concurrency
> network server actually works, three ways: blocking, and event-driven I/O.

## Concepts demonstrated

- **TCP networking** — a server that speaks a simple line protocol over sockets.
- **Event-driven I/O** — a single-threaded `kqueue`/`epoll` event loop serving
  many clients concurrently (no thread-per-client, no `fork`-per-client).
- **Data structures** — a hand-written hash table with separate chaining.
- **Key expiry** — `EXPIRE`/`TTL` with both passive and active expiration.
- **Persistence** — an append-only file (AOF): every write is logged and
  replayed on startup, so data survives restarts.
- **Memory management** — bounded memory with an LRU eviction policy.

## Build & run

```bash
make            # builds ./miniredis
./miniredis     # listens on port 6379
```

Talk to it from another terminal with plain `nc`:

```bash
$ nc 127.0.0.1 6379
PING
PONG
SET name bhavy
OK
GET name
bhavy
```

## Protocol

One command per line, space-separated (inline protocol). Replies are
line-based. (A binary-safe RESP protocol is a planned upgrade.)

| Command | Description | Status |
|---|---|---|
| `PING` | health check → `PONG` | ✅ |
| `ECHO <msg>` | echo a message | ✅ |
| `SET <key> <val>` | store a value | ✅ |
| `GET <key>` | fetch a value | ✅ |
| `DEL <key>` | delete a key | ⬜ Day 3 |
| `EXISTS <key>` | test for a key | ⬜ Day 3 |
| `EXPIRE <key> <sec>` | set a time-to-live | ⬜ Day 7 |
| `TTL <key>` | remaining time-to-live | ⬜ Day 7 |

## Roadmap

Four-week build plan (~15 hrs/week):

- **Week 1 — Foundations:** scaffold, inline protocol, hash table, `DEL`/`EXISTS`.
- **Week 2 — The event loop:** non-blocking sockets + `kqueue`/`epoll`,
  per-client read buffers → one thread, many concurrent clients.
- **Week 3 — Expiry & persistence:** `EXPIRE`/`TTL` (passive + active), AOF
  write + replay on startup.
- **Week 4 — Polish:** LRU eviction, benchmarking vs a blocking server, unit
  tests, docs.

## Design notes

- **Portability:** the event backend is `kqueue` on macOS/BSD and `epoll` on
  Linux; the rest of the server is platform-agnostic.
- **Why single-threaded?** Like real Redis, all command execution happens on
  one thread, so the in-memory data structures need no locking. Concurrency
  comes from multiplexing I/O, not from threads.

## License

MIT
