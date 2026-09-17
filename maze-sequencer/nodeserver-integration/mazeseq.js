// Redirects nodeServer's /mazeseq to the standalone Force Maze Sequencer web
// panel (a separate process/port -- see web/README or web/server.py). A
// real HTTP redirect, not a rendered <a href>, deliberately: home.js's own
// link renderer runs every URL through the legacy global escape(), which
// mangles the colons in an absolute "http://host:port" URL. Same pattern as
// force-acid's forceacid.js and ../../maze-voice/nodeserver-integration/
// forcemaze.js.
//
// NOT named "forcemazeseq" (or anything else starting with "forcemaze"):
// nodeServer's router (app/server.js) matches ENDPOINTS by
// `req.url.startsWith(e.PARAM)`, first match in array order wins - so a
// "/forcemazeseq" route registered after "/forcemaze" is unreachable, since
// every request for it also starts with "/forcemaze" and matches that
// earlier, wrong entry first. Hit this live (redirected to Maze Voice's
// port instead of this module's) before renaming to a route that shares no
// prefix with any existing one. See ../DESIGN.md.
//
// NOTE: the target IP is hardcoded below, matching this repo's other
// redirect endpoints (same Force device). If the Force's IP changes (no
// DHCP reservation set), update it here -- see ENDPOINTS.js's entry for
// "Maze Sequencer".
module.exports = { INIT };

function INIT(req, res) {
    res.writeHead(302, { Location: "http://192.168.1.187:8305/" });
    res.end();
}
