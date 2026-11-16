const http = require('node:http');
const fs = require('node:fs');
const path = require('node:path');
const root = path.resolve(__dirname, '../web');
const routes = { '/': 'index.html', '/wifi': 'wifi.html', '/mqtt': 'mqtt.html', '/app.js': 'app.js', '/app.css': 'app.css' };
function createServer() { return http.createServer((req, res) => {
  const file = routes[new URL(req.url, 'http://localhost').pathname];
  if (!file) { res.writeHead(404); res.end(); return; }
  const type = file.endsWith('.js') ? 'text/javascript' : file.endsWith('.css') ? 'text/css' : 'text/html';
  res.writeHead(200, { 'Content-Type': type + '; charset=utf-8' });
  res.end(fs.readFileSync(path.join(root, file)));
}); }
module.exports = {createServer};
if (require.main === module) createServer().listen(8765, '127.0.0.1');
