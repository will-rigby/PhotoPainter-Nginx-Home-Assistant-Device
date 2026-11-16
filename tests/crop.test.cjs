const {test} = require('node:test');
const assert = require('node:assert/strict');
const {cropRect, defaultCrop} = require('../web/app.js');
test('centered crops preserve the frame ratio without stretching', () => {
  for (const [w,h] of [[4000,3000],[3000,4000],[800,480],[5000,800]]) {
    const r = cropRect(w,h,defaultCrop());
    assert.ok(Math.abs(r.w / r.h - 5 / 3) < 1e-9);
    assert.ok(Math.abs(r.x + r.w / 2 - w / 2) < 1e-9);
    assert.ok(Math.abs(r.y + r.h / 2 - h / 2) < 1e-9);
  }
});
test('all rotations and edge drags stay inside the image', () => {
  for (const rotation of [0,90,180,270]) for (const zoom of [1,2,4])
    for (const x of [-100,0,.5,1,100]) for (const y of [-100,0,.5,1,100]) {
      const r = cropRect(3000,4000,{rotation,zoom,x,y});
      assert.ok(r.x >= 0 && r.y >= 0);
      assert.ok(r.x + r.w <= r.rw + 1e-9 && r.y + r.h <= r.rh + 1e-9);
      assert.ok(Math.abs(r.w/r.h - 5/3) < 1e-9);
    }
});
