# nodeServer integration

Not part of the `ForceMazeSeq` addon itself - these patch the **nodeServer**
addon (a separate, shared MockbaMod addon), which is what actually renders
the Modules page and the home-page quick-links. Same pattern as
`force-acid/nodeserver-integration/` and
`../../maze-voice/nodeserver-integration/` - see either for the fuller
writeup; this file only covers what's specific to this module.

## Modules page (`/moduler`) - no patch needed

Automatic: nodeServer's `moduler` endpoint scans every `AddOns/*/NSMODULE.json`
and lists whatever it finds. Once `addon/NSMODULE.json` is deployed inside
`AddOns/ForceMazeSeq/`, "Maze Sequencer" just appears there, with
start/stop + autolaunch-toggle controls. Unlike Maze Voice, toggling
autolaunch here is safe at any time - no `LD_PRELOAD`/`acvs` boot-race
concern (see `../DESIGN.md`), and `NSMODULE.json` already sets
`AUTOLAUNCHABLE: true`.

`NSMODULE.json`'s `ARGUMENTS` splits every flag and its value into separate
array entries (`moduler`'s spawn call does `JSN.ARGUMENTS.map(A => A.VALUE)`
straight into `spawn()`, one argv entry per array entry, not shell-split) -
see `../../maze-voice/nodeserver-integration/README.md`'s longer writeup of
this gotcha if you ever add an argument.

## Home page quick-link - two files to add

1. Copy `mazeseq.js` to nodeServer's `app/api/endpoints/mazeseq.js`.
2. Add this entry to `app/api/ENDPOINTS.js`'s exported array (after the
   "Maze Voice" entry is a reasonable place):

```js
    {
        // Maze Sequencer runs its own standalone server (not an
        // in-process nodeServer module -- see web/README or server.py).
        // URL/PARAM stay a plain relative path on purpose (home.js's
        // escape() call mangles absolute "http://host:port" URLs -- see
        // mazeseq.js); clicking this link hits nodeServer's own /mazeseq
        // route, which mazeseq.js immediately 302-redirects out to the
        // real panel.
        NAME: "Maze Sequencer",
        PATH: "./api/endpoints/mazeseq.js",
        PARAM: "/mazeseq",
        URL: "/mazeseq",
        HIDDEN: false,
        HOME: true,
        TARGET: "MAZESEQ"
    },
```

3. Restart nodeServer for the new route to be picked up. **`run_nodeserver.sh`
   has the same bug documented in `../../maze-voice/DESIGN.md` for this
   module's own `run_maze_seq.sh`**: it backgrounds `nodeserver -v` without
   redirecting its stdout, so a plain `ssh ... run_nodeserver.sh` over SSH
   hangs forever (the process itself starts fine - only the SSH client gets
   stuck waiting for that fd to close). Restart it like this instead:
   ```sh
   ssh root@<force-ip> '/media/662522/AddOns/run_nodeserver.sh kill'
   ssh root@<force-ip> 'mmPath=$(cat /dev/shm/.mmPath); $mmPath/AddOns/nodeServer/nodeserver -v > /tmp/nodeserver.log 2>&1 &'
   ```
   (Not something to fix in `run_nodeserver.sh` itself here - that script
   belongs to the shared nodeServer addon, out of scope for this module.)

## Route-naming gotcha: nodeServer's router matches by prefix, not exact URL

**Do not name a new route with an existing route's `PARAM` as a literal
prefix.** `app/server.js` dispatches with `req.url.startsWith(e.PARAM)`,
first match in `ENDPOINTS.js`'s array order wins. The route above was
originally named `/forcemazeseq` (mirroring `forceacid`/`forcemaze`'s
naming) and was silently unreachable: every request for it also starts with
the earlier-registered `/forcemaze` (Maze Voice), so the router
matched that entry first and redirected to the wrong port every time - no
error, no 404, just the wrong panel. Confirmed live, fixed by renaming to
`/mazeseq`, which shares no prefix with any existing route. See
`mazeseq.js`'s own header comment.

Both files target port **8305** (this module's web panel) - if you ever
change `web/server.py --port`, update `mazeseq.js`'s redirect target to
match.
