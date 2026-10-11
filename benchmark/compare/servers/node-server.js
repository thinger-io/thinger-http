// The routes of servers/thinger_server.cpp on Node.js built-in http module.
// Usage: node node-server.js <port> [workers]: workers > 1 uses the cluster module
const cluster = require('cluster');
const http = require('http');
const port = Number(process.argv[2] || 9080);
const workers = Number(process.argv[3] || 1);

if (workers > 1 && cluster.isPrimary) {
  for (let i = 0; i < workers; i++) cluster.fork();
} else {
  const params = /^\/v1\/users\/([^/]+)\/devices\/([^/]+)$/;
  http.createServer((req, res) => {
    if (req.method === 'GET' && req.url === '/hello') {
      res.writeHead(200, {'Content-Type': 'text/plain'});
      return res.end('Hello World!');
    }
    const m = req.method === 'GET' && params.exec(req.url);
    if (m) {
      res.writeHead(200, {'Content-Type': 'application/json'});
      return res.end(JSON.stringify({user: m[1], device: m[2], connected: true}));
    }
    if (req.method === 'POST' && req.url === '/v1/echo') {
      const chunks = [];
      req.on('data', c => chunks.push(c));
      req.on('end', () => {
        try {
          const body = JSON.parse(Buffer.concat(chunks).toString());
          res.writeHead(200, {'Content-Type': 'application/json'});
          res.end(JSON.stringify(body));
        } catch (e) {
          res.writeHead(400); res.end();
        }
      });
      return;
    }
    res.writeHead(404); res.end();
  }).listen(port);
}
