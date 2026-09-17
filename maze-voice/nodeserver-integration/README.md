# nodeServer integration

Not part of the `ForceMazeVoice` addon itself - these patch the **nodeServer**
addon (a separate, shared MockbaMod addon), which is what actually renders
the Modules page and the home-page quick-links. `force-acid` did the
equivalent (`api/endpoints/forceacid.js`, an `ENDPOINTS.js` entry) directly
against a live nodeServer install without versioning it anywhere - this
folder exists so the same patch for Maze Voice doesn't get silently lost the
same way.

## Modules page (`/moduler`) - no patch needed

Automatic: nodeServer's `moduler` endpoint (`api/endpoints/moduler/index.js`)
scans every `AddOns/*/NSMODULE.json` and lists whatever it finds, with
start/stop + autolaunch-toggle controls driven entirely by that file. Once
`addon/NSMODULE.json` is deployed inside `AddOns/ForceMazeVoice/`, "Maze
Voice" just appears there - see that file's own `DESCRIPTION` for the
one caveat (toggling RUNNING there starts/stops `maze_host` only, it doesn't
arm the LD_PRELOAD tap or restart `acvs`).

Gotcha found and fixed while wiring this up: `moduler`'s spawn call does
`JSN.ARGUMENTS.map(A => A.VALUE)` and passes that straight to `spawn()` -
each `ARGUMENTS[].VALUE` becomes ONE argv entry, not shell-split. A value
like `"--module-dir /path"` would arrive at `maze_host` as a single
unparseable string. `NSMODULE.json` splits every flag and its value into
separate array entries for this reason.

## Home page quick-link - two files to add

1. Copy `forcemaze.js` to nodeServer's
   `app/api/endpoints/forcemaze.js`.
2. Add this entry to `app/api/ENDPOINTS.js`'s exported array (after the
   "Acid" entry is a reasonable place):

```js
    {
        // Maze Voice runs its own standalone server (not an in-process
        // nodeServer module -- see force-maze/web/README or server.py). URL/
        // PARAM stay a plain relative path on purpose (home.js's escape()
        // call mangles absolute "http://host:port" URLs -- see
        // forcemaze.js); clicking this link hits nodeServer's own
        // /forcemaze route, which forcemaze.js immediately 302-redirects
        // out to the real panel.
        NAME: "Maze Voice",
        PATH: "./api/endpoints/forcemaze.js",
        PARAM: "/forcemaze",
        URL: "/forcemaze",
        HIDDEN: false,
        HOME: true,
        TARGET: "FORCEMAZE"
    },
```

3. Restart nodeServer for the new route to be picked up (it does not need
   `acvs`/MPC touched at all - a plain process kill+relaunch of nodeServer's
   own `server.js`, or `run_nodeserver.sh kill` then re-run).

Both files target port **8304** (force-maze's web panel) - if you ever
change `web/server.py --port`, update `forcemaze.js`'s redirect target to
match.
