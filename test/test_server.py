#!/usr/bin/env python3
"""Integration tests for the cpp-http-server (httpd) binary.

Builds the server with CMake, starts it on a test port serving a generated
fixture docroot, and asserts the full required behavior matrix. Exits 0 only
if every check passes; exits 1 with a clear message on any failure.

Stdlib only — no pytest, no third-party dependencies.
"""

import concurrent.futures
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD_DIR = os.path.join(ROOT, "build")
BINARY = os.path.join(BUILD_DIR, "httpd")
TEST_PORT = 18080

FAILURES = []


def check(name, cond, detail=""):
    status = "PASS" if cond else "FAIL"
    print(f"[{status}] {name}" + (f" — {detail}" if detail and not cond else ""))
    if not cond:
        FAILURES.append(f"{name}: {detail}")


def build_server():
    print("== building ==")
    os.makedirs(BUILD_DIR, exist_ok=True)
    r = subprocess.run(
        ["cmake", "-S", ROOT, "-B", BUILD_DIR, "-DCMAKE_BUILD_TYPE=Release"],
        capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout + r.stderr)
        sys.exit("FATAL: cmake configure failed")
    r = subprocess.run(["cmake", "--build", BUILD_DIR, "-j"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout + r.stderr)
        sys.exit("FATAL: build failed (must be clean under -Wall -Wextra -Werror)")
    if not os.path.isfile(BINARY):
        sys.exit("FATAL: httpd binary not produced")
    print("build OK:", BINARY)


def make_fixtures():
    """Returns (tmpdir, docroot). A 'secret.txt' is placed OUTSIDE the docroot
    so traversal tests have something real to try to reach."""
    tmpdir = tempfile.mkdtemp(prefix="httpd-test-")
    docroot = os.path.join(tmpdir, "docroot")
    os.makedirs(os.path.join(docroot, "empty-dir"))
    files = {
        "index.html": b"<html><body><h1>hello</h1></body></html>",
        "hello.txt": b"hello world\n",
        "style.css": b"body { color: red; }\n",
        "app.js": b"console.log('hi');\n",
        "data.json": b'{"a": 1}\n',
        "pixel.png": bytes.fromhex(
            "89504e470d0a1a0a0000000d49484452000000010000000108060000001f15c489"
            "0000000d49444154789c626001000000ffff03000006000557bfabd400000000"
            "49454e44ae426082"),
    }
    for name, data in files.items():
        with open(os.path.join(docroot, name), "wb") as f:
            f.write(data)
    with open(os.path.join(tmpdir, "secret.txt"), "wb") as f:
        f.write(b"top secret\n")
    return tmpdir, docroot, files


def raw_request(port, data, timeout=5.0):
    """Send raw bytes, read until the server closes the connection, and parse
    the response into (status, headers, body)."""
    s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    try:
        s.sendall(data)
        chunks = []
        while True:
            try:
                c = s.recv(65536)
            except socket.timeout:
                break
            if not c:
                break
            chunks.append(c)
    finally:
        s.close()
    raw = b"".join(chunks)
    head, _, body = raw.partition(b"\r\n\r\n")
    lines = head.split(b"\r\n")
    status = int(lines[0].split(b" ")[1])
    headers = {}
    for ln in lines[1:]:
        k, _, v = ln.partition(b":")
        headers[k.strip().lower().decode("latin1")] = v.strip().decode("latin1")
    return status, headers, body


def get(port, path, version="HTTP/1.1", method="GET", extra_headers=""):
    req = (f"{method} {path} {version}\r\n"
           f"Host: 127.0.0.1:{port}\r\n{extra_headers}\r\n").encode()
    return raw_request(port, req)


def wait_for_server(port, timeout=10.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=1.0)
            s.close()
            return True
        except OSError:
            time.sleep(0.1)
    return False


def main():
    build_server()
    tmpdir, docroot, files = make_fixtures()
    proc = subprocess.Popen(
        [BINARY, "--port", str(TEST_PORT), "--docroot", docroot, "--threads", "4"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        if not wait_for_server(TEST_PORT):
            sys.exit("FATAL: server did not start listening")
        print("== running tests ==")

        # 1. Basic 200 with correct body + MIME + Content-Length
        for name, ctype in [("hello.txt", "text/plain"),
                            ("style.css", "text/css"),
                            ("app.js", "text/javascript"),
                            ("data.json", "application/json"),
                            ("pixel.png", "image/png"),
                            ("index.html", "text/html")]:
            st, h, body = get(TEST_PORT, f"/{name}")
            check(f"GET /{name} -> 200", st == 200, f"got {st}")
            check(f"GET /{name} body", body == files[name],
                  f"got {body[:40]!r}")
            check(f"GET /{name} content-type {ctype}",
                  h.get("content-type", "").startswith(ctype),
                  f"got {h.get('content-type')!r}")
            check(f"GET /{name} content-length",
                  h.get("content-length") == str(len(files[name])),
                  f"got {h.get('content-length')!r}")

        # 2. Directory request serves index.html
        st, h, body = get(TEST_PORT, "/")
        check("GET / serves index.html", st == 200 and body == files["index.html"],
              f"got {st} body={body[:30]!r}")

        # 3. HEAD: headers only, correct Content-Length, empty body
        st, h, body = get(TEST_PORT, "/hello.txt", method="HEAD")
        check("HEAD /hello.txt -> 200", st == 200, f"got {st}")
        check("HEAD /hello.txt empty body", body == b"", f"got {body!r}")
        check("HEAD /hello.txt content-length",
              h.get("content-length") == str(len(files["hello.txt"])),
              f"got {h.get('content-length')!r}")

        # 4. 404 + Content-Length present on errors
        st, h, body = get(TEST_PORT, "/nope.txt")
        check("GET /nope.txt -> 404", st == 404, f"got {st}")
        check("404 has content-length", "content-length" in h)

        # 5. Path traversal attempts -> 403 (must not leak ../secret.txt)
        with open(os.path.join(tmpdir, "secret.txt"), "rb") as f:
            secret = f.read()
        for label, path in [
            ("plain ../", "/../secret.txt"),
            ("encoded %2e%2e", "/%2e%2e/secret.txt"),
            ("encoded %2E%2E", "/%2E%2E/%2E%2E/secret.txt"),
            ("encoded slash ..%2f", "/..%2fsecret.txt"),
            ("nested", "/sub/%2e%2e/%2e%2e/secret.txt"),
        ]:
            st, h, body = get(TEST_PORT, path)
            check(f"traversal {label} -> 403", st == 403, f"got {st}")
            check(f"traversal {label} body not leaked", body != secret)

        # 6. Directory without index.html -> 403 (no listing)
        st, h, body = get(TEST_PORT, "/empty-dir/")
        check("GET /empty-dir/ -> 403", st == 403, f"got {st}")

        # 7. 405 for POST, with Allow header
        st, h, body = get(TEST_PORT, "/hello.txt", method="POST")
        check("POST /hello.txt -> 405", st == 405, f"got {st}")
        check("405 Allow header", "get" in h.get("allow", "").lower(),
              f"got {h.get('allow')!r}")

        # 8. Bad HTTP version -> 505
        st, h, body = get(TEST_PORT, "/hello.txt", version="HTTP/2.0")
        check("HTTP/2.0 -> 505", st == 505, f"got {st}")

        # 9. Malformed request -> 400
        st, h, body = raw_request(TEST_PORT, b"GARBAGE\r\n\r\n")
        check("malformed request -> 400", st == 400, f"got {st}")

        # 10. Oversized headers (>8KB) -> 431
        big = b"GET /hello.txt HTTP/1.1\r\nHost: x\r\nX-Pad: " + b"A" * 9000 + b"\r\n\r\n"
        st, h, body = raw_request(TEST_PORT, big)
        check("oversized headers -> 431", st == 431, f"got {st}")

        # 11. Query strings are ignored
        st, h, body = get(TEST_PORT, "/hello.txt?x=1&y=2")
        check("query string ignored", st == 200 and body == files["hello.txt"],
              f"got {st}")

        # 12. Connection: close on every response
        st, h, body = get(TEST_PORT, "/hello.txt")
        check("Connection: close", h.get("connection") == "close",
              f"got {h.get('connection')!r}")

        # 13. 50 concurrent GETs all succeed with correct bodies
        def fetch_one(i):
            st, h, body = get(TEST_PORT, "/hello.txt")
            return st == 200 and body == files["hello.txt"]

        with concurrent.futures.ThreadPoolExecutor(max_workers=50) as ex:
            results = list(ex.map(fetch_one, range(50)))
        check("50 concurrent GETs all 200 + correct body", all(results),
              f"{sum(results)}/50 ok")

        # 14. Graceful shutdown: SIGTERM -> exit 0
        proc.send_signal(signal.SIGTERM)
        try:
            rc = proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            rc = "timeout"
        check("SIGTERM -> clean exit 0", rc == 0, f"got {rc}")
        proc = None  # already reaped

    finally:
        if proc is not None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
        shutil.rmtree(tmpdir, ignore_errors=True)

    print()
    if FAILURES:
        print(f"{len(FAILURES)} FAILURE(S):")
        for f in FAILURES:
            print("  -", f)
        sys.exit(1)
    print("ALL TESTS PASSED")


if __name__ == "__main__":
    main()
