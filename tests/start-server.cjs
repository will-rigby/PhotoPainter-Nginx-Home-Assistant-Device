const {createServer} = require('./serve.cjs');
module.exports = async () => {
  const server = createServer();
  await new Promise((resolve, reject) => {
    server.once('error', reject); server.listen(8765, '127.0.0.1', resolve);
  });
  return async () => {
    server.closeAllConnections();
    await new Promise(resolve => server.close(resolve));
  };
};
