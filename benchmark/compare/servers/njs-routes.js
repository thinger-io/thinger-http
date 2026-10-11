// The routes of servers/thinger_server.cpp on nginx + njs
function hello(r) {
    r.headersOut['Content-Type'] = 'text/plain';
    r.return(200, 'Hello World!');
}
function params(r) {
    const m = r.uri.match(/^\/v1\/users\/([^/]+)\/devices\/([^/]+)$/);
    if (!m) { r.return(404); return; }
    r.headersOut['Content-Type'] = 'application/json';
    r.return(200, JSON.stringify({user: m[1], device: m[2], connected: true}));
}
function echo(r) {
    let body;
    try { body = JSON.parse(r.requestText); } catch (e) { r.return(400); return; }
    r.headersOut['Content-Type'] = 'application/json';
    r.return(200, JSON.stringify(body));
}
export default {hello, params, echo};
