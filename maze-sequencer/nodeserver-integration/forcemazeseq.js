// Redirects nodeServer's /forcemazeseq to the standalone Force Maze
// Sequencer web panel (a separate process/port -- see web/README or
// web/server.py). A real HTTP redirect, not a rendered <a href>,
// deliberately: home.js's own link renderer runs every URL through the
// legacy global escape(), which mangles the colons in an absolute
// "http://host:port" URL. Same pattern as force-acid's forceacid.js and
// ../../maze-voice/nodeserver-integration/forcemaze.js.
//
// NOTE: the target IP is hardcoded below, matching this repo's other
// redirect endpoints (same Force device). If the Force's IP changes (no
// DHCP reservation set), update it here -- see ENDPOINTS.js's entry for
// "Force Maze Sequencer".
module.exports = { INIT };

function INIT(req, res) {
    res.writeHead(302, { Location: "http://192.168.1.187:8305/" });
    res.end();
}
