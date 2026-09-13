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
`AddOns/ForceMazeSeq/`, "Force Maze Sequencer" just appears there, with
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

1. Copy `forcemazeseq.js` to nodeServer's `app/api/endpoints/forcemazeseq.js`.
2. Add this entry to `app/api/ENDPOINTS.js`'s exported array (after the
   "Force Maze Voice" entry is a reasonable place):

```js
    {
        // Force Maze Sequencer runs its own standalone server (not an
        // in-process nodeServer module -- see web/README or server.py).
        // URL/PARAM stay a plain relative path on purpose (home.js's
        // escape() call mangles absolute "http://host:port" URLs -- see
        // forcemazeseq.js); clicking this link hits nodeServer's own
        // /forcemazeseq route, which forcemazeseq.js immediately
        // 302-redirects out to the real panel.
        NAME: "Force Maze Sequencer",
        PATH: "./api/endpoints/forcemazeseq.js",
        PARAM: "/forcemazeseq",
        URL: "/forcemazeseq",
        HIDDEN: false,
        HOME: true,
        TARGET: "FORCEMAZESEQ"
    },
```

3. Restart nodeServer for the new route to be picked up (a plain process
   kill+relaunch of nodeServer's own `server.js`, or `run_nodeserver.sh kill`
   then re-run - does not need `acvs`/MPC touched).

Both files target port **8305** (this module's web panel) - if you ever
change `web/server.py --port`, update `forcemazeseq.js`'s redirect target to
match.
