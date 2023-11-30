# C++ Concurrent Server

A small C++ HTTP server used to study sockets, non-blocking I/O, and event-loop
server design.

Current version:

- Linux `epoll`-based event loop
- non-blocking client sockets
- per-connection request/response buffers
- worker pool for delayed responses
- bounded worker queue for backpressure
- `eventfd` wakeup when workers finish
- `SO_REUSEPORT` for multi-process worker mode
- request deadlines for slow worker tasks
- signal-aware shutdown path
- routes: `/health`, `/metrics`, `/echo`, `/slow`

## Build

```bash
make clean && make
```

## Run

```bash
./server
```

Multiple server processes can bind the same port through `SO_REUSEPORT`:

```bash
# terminal 1
./server

# terminal 2
./server
```

## Try It

```bash
curl http://127.0.0.1:8080/health
curl -X POST http://127.0.0.1:8080/echo -d 'hello'
curl http://127.0.0.1:8080/metrics
```

Smoke test, with `./server` already running:

```bash
./scripts/smoke_test.sh
```

`/slow` runs in the worker pool and has a request deadline:

```bash
time sh -c 'for i in $(seq 1 8); do curl -s http://127.0.0.1:8080/slow >/dev/null & done; wait'
```

The demo worker sleeps for 2s while the request deadline is 1s, so `/slow`
returns `504 Gateway Timeout`.

Limitations: minimal HTTP parsing, no keep-alive, no TLS, and Linux `epoll`.
