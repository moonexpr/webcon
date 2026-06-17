# x86_64 smoke test (conplex + webcon)

`webtest.sp` is a minimal end-to-end check of a built/installed conplex + webcon.
It registers a webcon request handler; an HTTP request to the game port then has
to be accepted by the game server, detected as HTTP and routed by **conplex**
(its `CSocketCreator::ProcessAccept` detour), handed to **webcon**, dispatched to
the handler, and answered — exercising both extensions together.

## Run

1. Build + install both extensions (see the build instructions in the PR).
2. Compile with SourceMod's `spcomp` (with `webcon.inc` on the include path) and
   put `webtest.smx` in `addons/sourcemod/plugins/`.
3. `sm plugins load webtest`, then from any host (webcon routes by the first URL
   path segment, matching the handler id):

   ```
   curl -L http://<server>:27015/e2etest
   ```

Expected: `E2E-OK conplex+webcon x64` (HTTP 200), served by the plugin handler.
A built-in `404 Not Found` instead means the request reached webcon but not the
handler (wrong path); a connection failure means conplex did not route it.
