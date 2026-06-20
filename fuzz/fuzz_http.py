#!/usr/bin/env python3
"""Raw-socket HTTP fuzzer for conplex + webcon on the game port."""

from __future__ import annotations

import argparse
import random
import socket
import string
import struct
import sys
import time
from dataclasses import dataclass, field
from typing import Callable, Iterable

DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 27999
READ_SIZE = 65536
CONNECT_TIMEOUT = 3.0
IO_TIMEOUT = 2.0


@dataclass
class CaseResult:
    category: str
    name: str
    outcome: str
    detail: str = ""
    response_preview: str = ""


@dataclass
class FuzzReport:
    results: list[CaseResult] = field(default_factory=list)
    health_before: str = ""
    health_after: str = ""
    server_alive_before: bool = False
    server_alive_after: bool = False
    elapsed_s: float = 0.0
    last_case: str = ""
    server_died_at: str = ""

    @property
    def counts(self) -> dict[str, int]:
        counts: dict[str, int] = {}
        for r in self.results:
            counts[r.outcome] = counts.get(r.outcome, 0) + 1
        return counts

    @property
    def anomalies(self) -> list[CaseResult]:
        boring = {"ok", "expected_close", "timeout", "no_response"}
        return [r for r in self.results if r.outcome not in boring]


def recv_some(sock: socket.socket, timeout: float = IO_TIMEOUT) -> bytes:
    sock.settimeout(timeout)
    chunks: list[bytes] = []
    try:
        while True:
            data = sock.recv(READ_SIZE)
            if not data:
                break
            chunks.append(data)
            if len(b"".join(chunks)) >= READ_SIZE:
                break
    except socket.timeout:
        pass
    except ConnectionResetError:
        return b""
    except OSError:
        return b""
    return b"".join(chunks)


def classify_response(data: bytes) -> tuple[str, str]:
    if not data:
        return "no_response", "empty"

    text = data.decode("latin-1", errors="replace")
    head = text[:512]

    if data.startswith(b"HTTP/"):
        line = text.split("\r\n", 1)[0]
        parts = line.split(" ", 2)
        if len(parts) >= 2 and parts[1].isdigit():
            code = int(parts[1])
            if 200 <= code < 600:
                return "ok", f"HTTP {code}"
        return "weird_http", line[:120]

    if b"E2E-OK" in data:
        return "ok", "body without parseable status"

    if len(data) < 8 and all(b < 128 for b in data):
        return "garbage_small", repr(data[:32])

    return "unexpected", repr(data[:80])


def send_raw(
    host: str,
    port: int,
    payload: bytes,
    *,
    read_after: bool = True,
    io_timeout: float = IO_TIMEOUT,
    shutdown_after_send: bool = False,
) -> tuple[str, str, str]:
    sock = socket.create_connection((host, port), timeout=CONNECT_TIMEOUT)
    try:
        if shutdown_after_send:
            sock.sendall(payload)
            try:
                sock.shutdown(socket.SHUT_WR)
            except OSError:
                pass
            data = recv_some(sock, io_timeout) if read_after else b""
        else:
            sock.sendall(payload)
            data = recv_some(sock, io_timeout) if read_after else b""
    except ConnectionResetError:
        return "reset", "connection reset by peer", ""
    except BrokenPipeError:
        return "reset", "broken pipe", ""
    except socket.timeout:
        return "timeout", "connect/send/read timeout", ""
    except OSError as exc:
        return "error", str(exc), ""
    finally:
        sock.close()

    if not read_after:
        return "sent", "payload sent", ""

    outcome, detail = classify_response(data)
    preview = data[:200].decode("latin-1", errors="replace").replace("\r", "\\r").replace("\n", "\\n")
    return outcome, detail, preview


def http_request(
    method: str,
    path: str,
    *,
    version: str = "HTTP/1.1",
    headers: dict[str, str] | None = None,
    body: bytes = b"",
) -> bytes:
    headers = headers or {}
    if body and "Content-Length" not in headers:
        headers["Content-Length"] = str(len(body))

    lines = [f"{method} {path} {version}"]
    lines.extend(f"{k}: {v}" for k, v in headers.items())
    head = "\r\n".join(lines).encode("latin-1", errors="replace") + b"\r\n\r\n"
    return head + body


def rand_text(n: int) -> str:
    alphabet = string.ascii_letters + string.digits + "/?&=%-_."
    return "".join(random.choice(alphabet) for _ in range(n))


def health_check(host: str, port: int) -> str:
    payload = http_request("GET", "/e2etest/", headers={"Host": f"{host}:{port}"})
    try:
        outcome, detail, preview = send_raw(host, port, payload, io_timeout=3.0)
    except (ConnectionRefusedError, OSError) as exc:
        return f"unreachable: {exc}"
    if outcome == "ok" and "E2E-OK" in preview:
        return "healthy"
    return f"{outcome}: {detail} | {preview[:80]}"


def run_case(
    report: FuzzReport,
    category: str,
    name: str,
    runner: Callable[[], tuple[str, str, str]],
) -> None:
    outcome, detail, preview = runner()
    report.results.append(
        CaseResult(category, name, outcome, detail, preview)
    )


def gen_cases(host: str, port: int, seed: int) -> Iterable[tuple[str, str, Callable[[], tuple[str, str, str]]]]:
    rng = random.Random(seed)
    host_hdr = f"{host}:{port}"

    def case(category: str, name: str, payload: bytes, **kwargs):
        def run():
            return send_raw(host, port, payload, **kwargs)
        return category, name, run

    # --- Valid baseline variants ---
    yield case("valid", "e2etest_slash", http_request("GET", "/e2etest/"))
    yield case("valid", "index", http_request("GET", "/"))
    yield case("valid", "redirect_no_slash", http_request("GET", "/e2etest"))
    yield case("valid", "unknown_handler", http_request("GET", "/nope/"))
    yield case("valid", "head_e2etest", http_request("HEAD", "/e2etest/"))

    # --- Path fuzz ---
    path_cases = [
        "/" + "A" * 8192,
        "/" + "e2etest/" + "x" * 16384,
        "/..%2f..%2fetc%2fpasswd",
        "/.%2e/%2e%2e/%2e%2e/",
        "/e2etest/%00",
        "/e2etest/%00/",
        "/e2etest/\x00hidden",
        "/e2etest/..;/",
        "/e2etest/%2e%2e/",
        "/e2etest/%252e%252e/",
        "/e2etest/" + "../" * 40,
        "/e2etest/?" + "q=" + "x" * 8000,
        "//e2etest//",
        "/./e2etest/./",
        "/e2etest/%",
        "/e2etest/%zz",
        "/\xe2\x98\x83/",
        "/e2etest/\r\nInjected: yes",
        "/e2etest/\n",
        "/E2ETEST/",
        "/e2etest",
        "/e2etest/.",
        "/e2etest/..",
        "/%65%32%65%74%65%73%74/",
    ]
    for i, path in enumerate(path_cases):
        yield case("path", f"path_{i}", http_request("GET", path, headers={"Host": host_hdr}))

    # --- Method fuzz ---
    for method in [
        "",
        "GET",
        "POST",
        "PUT",
        "DELETE",
        "PATCH",
        "OPTIONS",
        "TRACE",
        "CONNECT",
        "FOOBAR",
        "GETGETGET",
        "A" * 256,
        "GE\r\nT",
        "GET\n",
        "\x00GET",
    ]:
        safe = method.encode("latin-1", errors="replace").decode("latin-1").replace("\r", "R").replace("\n", "N")[:24]
        yield case("method", f"method_{safe}", http_request(method, "/e2etest/", headers={"Host": host_hdr}))

    # --- Version / request-line fuzz ---
    bad_lines = [
        b"GET /e2etest/\r\n\r\n",
        b"GET /e2etest/ HTTP/1.1\r\n\r\n",
        b"GET /e2etest/ HTTP/9.9\r\nHost: x\r\n\r\n",
        b"GET /e2etest/ HTTP/1.1\nHost: x\n\n",
        b"GET /e2etest/ HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\n1234",
        b"GET /e2etest/ HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n",
        b"GET /e2etest/ HTTP/1.1\r\nHost: x\r\nContent-Length: 999999\r\n\r\n" + b"x" * 64,
        b"GET / HTTP/1.1\r\nHost: " + (b"A" * 16384) + b"\r\n\r\n",
        b"GET /e2etest/ HTTP/1.1\r\nHost: " + host_hdr.encode() + b"\r\nX-" + (b"Y: z\r\n" * 500) + b"\r\n",
        b"GET /e2etest/ HTTP/1.1\r\nHost: " + host_hdr.encode() + b"\r\n\r\nGET / HTTP/1.1\r\nHost: x\r\n\r\n",
        b"\x00GET /e2etest/ HTTP/1.1\r\nHost: x\r\n\r\n",
        b"GET /e2etest/ HTTP/1.1\r\nHost: x\r\nFoo: bar\r\n\r\n" + b"\x00" * 32,
    ]
    for i, payload in enumerate(bad_lines):
        yield case("request_line", f"line_{i}", payload)

    # --- Header smuggling / injection ---
    header_sets = [
        {"Host": host_hdr, "Content-Length": "0", "Transfer-Encoding": "chunked"},
        {"Host": host_hdr, "Transfer-Encoding": "chunked", "Transfer-encoding": "identity"},
        {"Host": "evil\r\nX-Injected: 1"},
        {"Host": host_hdr, "Connection": "close\r\nX-Injected: 1"},
        {"Host": host_hdr, "X-Long": "A" * 32768},
        {"Host": host_hdr, "Cookie": "c=" + "x" * 16000},
        {"Host": host_hdr, "\r\nX-Injected": "1"},
    ]
    for i, hdrs in enumerate(header_sets):
        yield case("headers", f"hdr_{i}", http_request("GET", "/e2etest/", headers=hdrs))

    # --- Non-HTTP / detector edge (conplex should reject or ignore) ---
    non_http = [
        b"\x16\x03\x01\x00\x05\x01\x00\x00\x01\x00",  # TLS-ish
        b"RCON hello world\n",
        b"\xff\xfeGET / HTTP/1.1\r\n\r\n",
        b"GET",  # partial
        b"GET ",
        b"GET /e2etest/ HTTP/1.1\r\n",
        os_random_bytes(rng, 64),
        os_random_bytes(rng, 512),
        os_random_bytes(rng, 4096),
    ]
    for i, payload in enumerate(non_http):
        yield case("non_http", f"blob_{i}", payload, read_after=True, io_timeout=1.0)

    # --- Partial / slow send / abrupt close ---
    def partial_then_close():
        sock = socket.create_connection((host, port), timeout=CONNECT_TIMEOUT)
        try:
            sock.sendall(b"GET /e2etest/ HTTP/1.1\r\nHost: " + host_hdr.encode())
            time.sleep(0.05)
            return "expected_close", "partial request left open then closed", ""
        finally:
            sock.close()

    yield ("connection", "partial_no_crlf", partial_then_close)

    def send_half_body():
        sock = socket.create_connection((host, port), timeout=CONNECT_TIMEOUT)
        try:
            sock.sendall(
                b"POST /e2etest/ HTTP/1.1\r\n"
                b"Host: " + host_hdr.encode() + b"\r\n"
                b"Content-Length: 100000\r\n\r\n"
                b"AAAA"
            )
            data = recv_some(sock, 1.0)
            outcome, detail = classify_response(data)
            preview = data[:120].decode("latin-1", errors="replace")
            return outcome, detail, preview
        finally:
            sock.close()

    yield ("connection", "short_post_body", send_half_body)

    def pipeline():
        payload = (
            http_request("GET", "/e2etest/", headers={"Host": host_hdr, "Connection": "keep-alive"})
            + http_request("GET", "/", headers={"Host": host_hdr, "Connection": "close"})
        )
        return send_raw(host, port, payload, io_timeout=3.0)

    yield ("connection", "pipeline_two", pipeline)

    # --- Randomized paths ---
    for i in range(40):
        path = "/" + rand_text(rng.randint(1, 120))
        yield case("random", f"rand_path_{i}", http_request("GET", path, headers={"Host": host_hdr}))

    # --- Stress burst ---
    def burst(n: int = 50):
        outcomes: dict[str, int] = {}
        for _ in range(n):
            payload = http_request("GET", "/e2etest/", headers={"Host": host_hdr})
            outcome, _, _ = send_raw(host, port, payload, io_timeout=1.5)
            outcomes[outcome] = outcomes.get(outcome, 0) + 1
        summary = ", ".join(f"{k}={v}" for k, v in sorted(outcomes.items()))
        worst = "ok" if outcomes.get("ok", 0) >= n // 2 else "stress_degraded"
        return worst, summary, ""

    yield ("stress", "burst_50_e2etest", burst)


def os_random_bytes(rng: random.Random, n: int) -> bytes:
    return bytes(rng.randint(0, 255) for _ in range(n))


def process_alive(name: str = "srcds_win64") -> bool:
    if sys.platform == "win32":
        import subprocess

        out = subprocess.check_output(
            ["tasklist", "/FI", f"IMAGENAME eq {name}.exe", "/NH"],
            stderr=subprocess.DEVNULL,
            text=True,
            errors="replace",
        )
        return name in out.lower()
    return False


def print_report(report: FuzzReport) -> None:
    print(f"Health before: {report.health_before}")
    print(f"Health after:  {report.health_after}")
    print(f"Server alive:  before={report.server_alive_before} after={report.server_alive_after}")
    print(f"Elapsed:       {report.elapsed_s:.1f}s")
    print(f"Total cases:   {len(report.results)}")
    if report.server_died_at:
        print(f"Server died during: {report.server_died_at} (last case: {report.last_case})")
    print("Outcomes:")
    for outcome, count in sorted(report.counts.items(), key=lambda x: (-x[1], x[0])):
        print(f"  {outcome:16} {count}")

    if report.anomalies:
        print("\nNotable anomalies:")
        for r in report.anomalies[:40]:
            print(f"  [{r.category}] {r.name}: {r.outcome} ({r.detail})")
            if r.response_preview:
                print(f"    preview: {r.response_preview[:100]}")
        if len(report.anomalies) > 40:
            print(f"  ... and {len(report.anomalies) - 40} more")
    else:
        print("\nNo notable anomalies outside expected close/timeout/no_response.")


def main() -> int:
    parser = argparse.ArgumentParser(description="Fuzz conplex/webcon HTTP on game port")
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--seed", type=int, default=0xE2E)
    parser.add_argument("--category", default="", help="Run only one category")
    args = parser.parse_args()

    report = FuzzReport()
    report.server_alive_before = process_alive()
    report.health_before = health_check(args.host, args.port)

    start = time.time()
    for category, name, runner in gen_cases(args.host, args.port, args.seed):
        if args.category and category != args.category:
            continue
        report.last_case = f"[{category}] {name}"
        if not process_alive():
            report.server_died_at = report.last_case
            break
        try:
            run_case(report, category, name, runner)
        except (ConnectionRefusedError, OSError) as exc:
            report.results.append(
                CaseResult(category, name, "connect_fail", str(exc))
            )
            if not process_alive():
                report.server_died_at = report.last_case
                break
    report.elapsed_s = time.time() - start

    time.sleep(1.0)
    report.server_alive_after = process_alive()
    report.health_after = health_check(args.host, args.port)

    print_report(report)

    if not report.server_alive_after:
        return 2
    if report.health_after != "healthy":
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
