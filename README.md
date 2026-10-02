# cpp-http-server

A multithreaded HTTP/1.1 static file server written from scratch in modern C++17.
Zero external dependencies — just POSIX sockets and the C++ standard library.

This is a portfolio project by **Jordan Nix**, a Computer Science sophomore,
built to demonstrate systems programming: sockets, threads, synchronization,
protocol parsing, and defensive input handling.

## Features

- Serves static files from a configurable document root (`GET` and `HEAD`)
- Fixed-size thread pool with a FIFO work queue
- HTTP/1.1 request parsing (request line + headers)
- MIME type detection by file extension (html, css, js, json, png, jpg, txt, …)
- `index.html` served automatically for directory requests
- Correct status codes: `200`, `400`, `403` (path traversal), `404`,
  `405` (non-GET/HEAD), `431` (oversized headers), `505` (bad HTTP version)
- `Content-Length` on every response, including errors
- Access log to stdout: `peer "METHOD /path" status bytes ms`
- Graceful shutdown on `SIGINT`/`SIGTERM`: stops accepting, drains the work
  queue, joins all threads, exits 0
- Security: rejects path traversal (`..`, absolute paths, URL-encoded variants
  like `%2e%2e` / `..%2f`) with `403`; jails symlinks inside the docroot;
  caps request headers at 8 KiB (`431`); refuses files over 100 MiB (`500`)

## Build

Requires a C++17 compiler and CMake (tested with g++ 13 and CMake 3.28).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
# binary: build/httpd
```

Compiles cleanly under `-Wall -Wextra -Werror`.

## Run

```sh
./build/httpd --port 8080 --docroot ./public --threads 8
```

## CLI reference

| Flag          | Default              | Description                              |
|---------------|----------------------|------------------------------------------|
| `--port N`    | `8080`               | TCP port to listen on (1–65535)          |
| `--docroot P` | `./public`           | Directory to serve (must exist)          |
| `--threads N` | hardware concurrency | Worker thread pool size (1–128)          |
| `--help, -h`  | —                    | Print usage and exit                     |

Example:

```sh
mkdir -p public && echo "<h1>hi</h1>" > public/index.html
./build/httpd --port 8080 --docroot ./public --threads 4
curl -i http://127.0.0.1:8080/
# Ctrl-C shuts down gracefully
```

## Architecture

```
                ┌──────────────┐
                │  main thread │  CLI parsing, signal setup (SIGINT/SIGTERM,
                └──────┬───────┘  SIGPIPE ignored)
                       │ run()
                ┌──────▼───────┐
                │ accept loop  │  select() on listen socket + self-pipe.
                │  (1 thread)  │  A signal writes one byte to the pipe to
                └──────┬───────┘  wake select() — no unsafe calls in handlers.
                       │ accepted fd
                ┌──────▼───────┐
                │  work queue  │  mutex + condition_variable FIFO of fds
                └──────┬───────┘
                       │ pop
        ┌──────────────┼──────────────┐
        ▼              ▼              ▼
   ┌────────┐    ┌────────┐    ┌────────┐
   │worker 1│    │worker 2│ …  │worker N│  read head → parse → route →
   └────────┘    └────────┘    └────────┘  serialize → send → close → log
```

**Connection handling** (`src/server.cpp`, `src/http.cpp`): each worker reads
up to 8 KiB of request head, parses the request line and headers, routes the
request against the canonicalized docroot, builds the response, sends it with
`send()` in a loop (partial writes handled, `MSG_NOSIGNAL`), closes the
connection, and writes one access-log line (mutex-guarded so lines never
interleave).

**Shutdown** (`Server::stop()` / `run()`): the signal handler only sets an
atomic flag and writes to the self-pipe — both async-signal-safe. The accept
loop exits, the listening socket closes, `ThreadPool::shutdown()` lets queued
connections drain, then every worker joins.

**Security model** (`handle_request`): the target is percent-decoded once,
then split into path segments; any `..` segment is rejected with `403`
*before* touching the filesystem. If the file exists, `realpath()` re-checks
that the resolved path stays under the docroot, which jails symlinks pointing
outside. Query strings are stripped and ignored.

## Protocol notes and honest limitations

- **HTTP/1.1 only** (`1.0` also accepted); anything else gets `505`.
- **No keep-alive**: every response carries `Connection: close` and the
  connection is closed after one request. Simpler, correct, and fine for a
  static file server at this scale.
- **No TLS, no chunked transfer encoding, no range requests, no compression.**
- Bodies on `GET`/`HEAD` requests are ignored; the request head is capped at
  8 KiB.
- Directory listings are not implemented — a directory without `index.html`
  returns `403`.

## Tests

Integration tests in `test/test_server.py` (Python 3, stdlib only — no pytest).
The script builds the server, starts it on port 18080 with a generated fixture
docroot, and asserts:

- `200` + exact body, MIME type, and `Content-Length` for html/css/js/json/png/txt
- `GET /` serves `index.html`; `HEAD` returns headers with an empty body
- `404` for missing files; `403` for `/../`, `%2e%2e`, `%2E%2E`, `..%2f`,
  and nested traversals (verifying the outside-docroot file is never leaked)
- `403` for directories without `index.html`
- `405` + `Allow` header for `POST`; `505` for `HTTP/2.0`; `400` for malformed
  requests; `431` for headers over 8 KiB
- 50 concurrent `GET`s all succeeding with correct bodies
- `SIGTERM` produces a clean exit 0

```sh
python3 test/test_server.py
```

The script exits nonzero with a clear message on any failure.

## Project layout

```
cpp-http-server/
├── CMakeLists.txt
├── src/
│   ├── main.cpp         # CLI parsing, signal setup, startup
│   ├── server.hpp/.cpp  # accept loop, self-pipe shutdown, connection handling
│   ├── thread_pool.hpp  # fixed-size pool + FIFO work queue (header-only)
│   ├── http.hpp/.cpp    # request parsing, routing, security, responses, logging
│   └── mime.hpp/.cpp    # extension → MIME type table
└── test/
    └── test_server.py   # integration suite (builds + exercises the server)
```
