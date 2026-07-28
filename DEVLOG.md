# miniredis — Project Documentation & Development Log

_Last updated: 2026-07-23_

This document records **why** this project exists, **what has been built so far**,
and **what comes next**. It's both a decision log and a status snapshot.

---

## 1. Background & decision

This project grew out of reviewing an existing university project — a
**Banking Management System in C** (client–server over TCP sockets, `fork()`
per connection, `fcntl()` record locking, POSIX semaphores, `crypt()` password
hashing, flat-file storage).

That project demonstrates solid systems fundamentals, but it's a **very common
template** (thousands of students build the same 4-role banking system), and it
had real weaknesses: a single static password salt, a fragile string-matching
wire protocol, flat `.txt` files as the "database", and unsafe input handling.

### Why build miniredis instead

The goal was a **distinctive, resume-ready systems project** that reuses the
existing socket/concurrency skills but is a clear level-up and not a copy of a
common template. A Redis-style in-memory key–value store fits perfectly:

- Keeps the networking/concurrency narrative from the coursework.
- Replaces `fork()`-per-connection with a **single-threaded event loop**
  (`kqueue`/`epoll`) — the technique real servers use, and a genuine step up.
- Has a clean, finishable scope: protocol → data structure → expiry →
  persistence → eviction.
- "I wrote a Redis-like server from scratch" is a strong interview opener.

### Timeline estimate

- **MVP** (event loop + hash table + core commands + persistence): ~2 weeks part-time.
- **Strong version** (+ expiry, LRU, benchmarks, tests): ~1 month part-time (~15 hrs/week).

---

## 2. What has been built so far ✅

A **working, building, version-controlled scaffold** — the Day 1–3 starting
point of the roadmap below.

### Repository
- **GitHub:** https://github.com/bhavy0949/miniredis (public)
- **Default branch:** `main`
- **First commit:** `Initial scaffold: blocking KV server with hash table`

### Files created

| File | Purpose |
|---|---|
| `dict.h` / `dict.c` | Hand-written **hash table** (separate chaining, djb2 hash). Includes an `expireAtMs` field on each entry, ready for the TTL work. Fully implemented. |
| `server.c` | The **TCP server**. Currently: blocking, one client at a time, inline text protocol, with `PING` / `ECHO` / `SET` / `GET` working. Contains clearly-marked `TODO` sections for the event loop, TTLs, and persistence. |
| `Makefile` | Build (`make`), run (`make run`), and `make clean`. Compiles warning-free with `-Wall -Wextra`. |
| `.gitignore` | Ignores the compiled binary, object files, and the future `.aof` file. |
| `README.md` | Product-facing overview: concepts, build/run instructions, protocol table, roadmap. |
| `DEVLOG.md` | This document. |

### Current capabilities

- TCP server listening on **port 6379**.
- **Inline protocol**: one command per line, space-separated. Human-readable
  replies (testable with plain `nc`).
- Working commands: `PING`, `ECHO`, `SET`, `GET`.
- A hash table backing the keyspace.

### Verified

- Builds cleanly (`make`) with no warnings.
- Smoke-tested manually: `PING → PONG`, `SET`/`GET` round-trip.

---

## 3. What comes next ⬜ (the 4-week roadmap)

| Week | Dates (2026) | Milestone | Status |
|---|---|---|---|
| 1 | Jul 23–29 | **Foundations** — learn the scaffold; implement `DEL` & `EXISTS` (warm-up) | 🚧 in progress |
| 2 | Jul 30–Aug 5 | **The event loop** — non-blocking sockets + `kqueue`/`epoll`, per-client buffers → many concurrent clients | ⬜ |
| 3 | Aug 6–12 | **Expiry & persistence** — `EXPIRE`/`TTL` (passive + active), AOF write + replay on startup | ⬜ |
| 4 | Aug 13–19 | **Polish** — LRU eviction, benchmark vs a blocking server, unit tests, docs | ⬜ |

### Command checklist

- [x] `PING`
- [x] `ECHO`
- [x] `SET`
- [x] `GET`
- [ ] `DEL` — Day 3 warm-up (stub in `server.c`)
- [ ] `EXISTS` — Day 3 warm-up (stub in `server.c`)
- [ ] `EXPIRE` / `TTL` — Day 7
- [ ] Event loop (`kqueue`/`epoll`) — Day 5–6
- [ ] AOF persistence — Day 9–10
- [ ] LRU eviction — Week 4

---

## 4. How to work on it

Everything lives in `~/Downloads/miniredis`. The workflow loop:

```bash
# 1. Build & run (terminal 1)
cd ~/Downloads/miniredis
make && ./miniredis

# 2. Test (terminal 2)
nc 127.0.0.1 6379
# then type: PING / SET x 5 / GET x

# 3. Commit progress
git add -A
git commit -m "describe what you changed"
git push
```

### Immediate next task
Implement `DEL` and `EXISTS` in `server.c` (look for the `cmd_del` / `cmd_exists`
stubs marked **"YOUR TURN"**). `dictDel` already does the deletion; you just wire
it up and reply `1\n` or `0\n`.

---

## 5. Design decisions on record

- **Single-threaded by design.** Like real Redis, all command execution runs on
  one thread, so the in-memory structures need no locks. Concurrency comes from
  **multiplexing I/O**, not threads — this is the core lesson of the project.
- **Portability.** The event backend will be `kqueue` on macOS (dev machine) and
  `epoll` on Linux; the rest of the server stays platform-agnostic. Keeping that
  boundary clean is itself a good talking point.
- **Inline protocol first, RESP later.** The simple line protocol is easy to test
  with `nc`. A binary-safe RESP parser (so values can contain spaces/binary) is a
  planned upgrade, not an MVP requirement.
