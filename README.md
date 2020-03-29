# C++ Concurrent Server

A TCP server built incrementally while studying Linux networking and C++
concurrency.

## Current Progress

- Create an IPv4 TCP socket.
- Bind to `127.0.0.1:8080`.
- Listen for incoming connections.
- Accept one TCP client.

## Build

```bash
make
```

## Run

```bash
./server
```

Connect from another terminal:

```bash
nc 127.0.0.1 8080
```

## Roadmap

- Echo client data.
- Handle partial reads and writes.
- Support concurrent clients.
- Add a bounded thread pool.
- Add graceful shutdown.
- Move to event-driven I/O.
