const { defineConfig, devices } = require('@playwright/test');
module.exports = defineConfig({
  testDir: './tests/browser',
  timeout: 30000,
  fullyParallel: true,
  workers: 4,
  use: { baseURL: 'http://127.0.0.1:8765', channel: 'msedge', trace: 'retain-on-failure' },
  projects: [
    { name: 'desktop', use: { viewport: { width: 1280, height: 900 } } },
    { name: 'mobile', use: { ...devices['Pixel 7'], defaultBrowserType: 'chromium' } }
  ],
  globalSetup: './tests/start-server.cjs'
});
