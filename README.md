# webcon

A pair of SourceMod extensions for the Source engine:

- **Webcon** — an embedded HTTP server, letting plugins handle web requests
  from inside the game server. Plugin API: [`webcon/webcon.inc`](webcon/webcon.inc).
- **Conplex** — a connection multiplexer that lets several protocols share a
  single listening port (e.g. RCON and HTTP on one socket), dispatching each
  connection to the right handler. Plugin API: [`conplex/conplex.inc`](conplex/conplex.inc).

Webcon embeds a vendored copy of [GNU libmicrohttpd](https://www.gnu.org/software/libmicrohttpd/);
see [`THIRD_PARTY`](THIRD_PARTY) for its license.

## Installation

Copy the extension binaries into your SourceMod install:

```
addons/sourcemod/extensions/webcon.ext.so      # .dll on Windows
addons/sourcemod/extensions/conplex.ext.so
```

A 64-bit server (`srcds -x64`) loads the matching binaries from the `x64/`
subdirectory:

```
addons/sourcemod/extensions/x64/webcon.ext.so
addons/sourcemod/extensions/x64/conplex.ext.so
```

To compile plugins against the extensions, drop their include files into
`addons/sourcemod/scripting/include/`.

## Building

Requires [AMBuild 2.2+](https://github.com/alliedmodders/ambuild) and a
SourceMod checkout. Both the 32- and 64-bit binaries build from one configure:

```sh
mkdir build && cd build
python ../configure.py --sm-path /path/to/sourcemod --targets x86,x86_64 --enable-optimize
ambuild
```

`configure.py` also accepts the standard SourceMod extension arguments
`--hl2sdk-root`, `--mms-path`, and `--sdks` (`present` by default). Build output
is packaged under `build/package`.
