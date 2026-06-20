# webcon / conplex HTTP fuzzer

Raw-socket fuzzer that hammers the conplex + webcon listener on the game
port with malformed and adversarial HTTP traffic. It was written to find
inputs that crash `srcds` through the webcon request path, and it is the
tooling behind the request-validation hardening on this branch (method /
URL / handler-id bounds checks, gating the engine RCON-auth path so fuzz
traffic can't reach it, and bounds checks in the HTTP protocol detector).

## Files

- `fuzz_http.py` — the fuzzer. One-shot, connects over a raw socket and
  sends generated cases across several categories (`valid`, `path`,
  `method`, `request_line`, `headers`, `non_http`, `connection`,
  `random`, `stress`). Classifies each response and reports anomalies.
- `fuzz_batch.py` — driver that runs `fuzz_http.py` one category at a
  time, restarting `srcds` between categories and recording whether the
  server survived each batch.

## Usage

The fuzzer needs a **live server** running webcon + conplex on the game
port (it does not start one itself):

```bash
python3 fuzz_http.py --host 127.0.0.1 --port 27999 --category method --seed 0xE2E
```

`fuzz_batch.py` automates start/stop across all categories, but its
top-of-file constants are **environment-specific** — edit them for your
setup before running:

- `TF_BAT`     — path to the srcds launch script (default `D:\srcds\tf2\tf.bat`)
- `CONSOLE`    — path to the server console log it polls for readiness
- `PROCESS`    — server process image name (default `srcds_win64.exe`)

It waits for the sentinel string `WEB-E2E` in the console log to decide
the server is up, so the test plugin needs to print that on load.

## What it found (pre-hardening)

Against an unhardened build, several categories took the server down —
`method`, `request_line`, and `non_http` each left `srcds` dead
(connection refused afterwards). Those crash paths are what the
validation on this branch closes; re-running the same batches against a
hardened build should leave the server alive after every category.
