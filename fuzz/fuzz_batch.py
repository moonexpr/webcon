#!/usr/bin/env python3
"""Run fuzz_http.py in batches, restarting srcds between categories if needed."""

from __future__ import annotations

import argparse
import subprocess
import sys
import time
from pathlib import Path

FUZZ = Path(__file__).with_name("fuzz_http.py")
TF_BAT = Path(r"D:\srcds\tf2\tf.bat")
CONSOLE = Path(r"D:\srcds\tf2\tf\console.log")
PROCESS = "srcds_win64.exe"

CATEGORIES = [
    "valid",
    "path",
    "method",
    "request_line",
    "headers",
    "non_http",
    "connection",
    "random",
    "stress",
]


def alive() -> bool:
    out = subprocess.check_output(
        ["tasklist", "/FI", f"IMAGENAME eq {PROCESS}", "/NH"],
        text=True,
        errors="replace",
    )
    return PROCESS.replace(".exe", "") in out.lower()


def stop_server() -> None:
    subprocess.run(
        ["taskkill", "/F", "/IM", PROCESS],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    time.sleep(2)


def start_server() -> None:
    subprocess.Popen(
        ["cmd", "/c", str(TF_BAT)],
        cwd=str(TF_BAT.parent),
        creationflags=subprocess.CREATE_NEW_CONSOLE,
    )
    for _ in range(90):
        time.sleep(2)
        if CONSOLE.exists() and "WEB-E2E" in CONSOLE.read_text(encoding="utf-8", errors="replace"):
            time.sleep(6)
            return
    raise RuntimeError("server did not reach WEB-E2E registration")


def run_batch(host: str, port: int, category: str, seed: int) -> int:
    env = dict(**__import__("os").environ)
    proc = subprocess.run(
        [sys.executable, str(FUZZ), "--host", host, "--port", str(port), "--seed", str(seed), "--category", category],
        text=True,
        capture_output=True,
    )
    print(proc.stdout)
    if proc.stderr:
        print(proc.stderr, file=sys.stderr)
    return proc.returncode


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=27999)
    parser.add_argument("--seed", type=int, default=0xE2E)
    args = parser.parse_args()

    summary: list[str] = []
    for cat in CATEGORIES:
        print(f"\n===== BATCH {cat} =====")
        stop_server()
        start_server()
        code = run_batch(args.host, args.port, cat, args.seed)
        died = not alive()
        summary.append(f"{cat}: exit={code} alive_after={not died}")
        if died:
            print(f"WARNING: server dead after batch {cat}")

    print("\n===== SUMMARY =====")
    for line in summary:
        print(line)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
