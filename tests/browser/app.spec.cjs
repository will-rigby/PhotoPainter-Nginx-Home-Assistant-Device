const {test, expect} = require('@playwright/test');

const photo = i => ({name: `photo_${i}.jpg`, revision: `s${i}`, thumbnail_revision: `t${i}`, cached: true, failed: false});
async function mockFrame(page, options = {}) {
  const calls = {uploads: 0, thumbs: 0, saves: [], shows: [], tokens: []};
  const status = {now: 10000, observed_at: 10000, stage: 'idle', active: null, pending: null,
    displayed: 'photo_1.jpg', last_job: 0, next_rotation_seconds: 1800, storage_total: 8000000000,
    storage_free: 6000000000, storage_at: 10000, sd_ready: true, mqtt_connected: true, mqtt_at: 10000,
    ap_mode: false, ip: '192.168.1.42', rssi: -48, hostname: 'photoframe', stopping: false, last_error: ''};
  await page.route('**/api/status', route => route.fulfill({json: status}));
  await page.route('**/api/config', route => route.fulfill({json: {wifi_ssid:'Home', hostname:'photoframe', mqtt_host:'broker', mqtt_port:1883, mqtt_user:'frame', wifi_password_saved:true, mqtt_password_saved:true}}));
  await page.route('**/api/photos?*', route => {
    const cursor = new URL(route.request().url()).searchParams.get('cursor');
    const index = cursor ? Number(cursor.split(':')[1]) : 0;
    const total = options.total ?? 1;
    const photos = Array.from({length: Math.min(24, total-index)}, (_, i) => photo(index+i));
    if (options.hostileName) photos[0].name = '<img src=x onerror=alert(1)>.jpg';
    return route.fulfill({json: {photos, generation: 1, next_cursor: index + photos.length < total ? `1:${index+photos.length}` : null}});
  });
  await page.route('**/thumb?file=*', route => route.fulfill({status:404, body:''}));
  await page.route('**/original?*', route => route.fulfill({status:404, body:''}));
  await page.route('**/upload?*', route => {
    calls.uploads++;
    calls.tokens.push(new URL(route.request().url()).searchParams.get('token'));
    if (options.lostResponse && calls.uploads === 1) return route.abort('failed');
    if (options.uploadError && calls.uploads === 1) return route.fulfill({status:507,json:{ok:false,error:'SD card is full'}});
    return route.fulfill({json:{ok:true,name:`stored_${calls.uploads}.jpg`}});
  });
  await page.route('**/thumb?name=*', route => {
    calls.thumbs++;
    if (options.thumbnailError && calls.thumbs === 1) return route.fulfill({status:507,json:{ok:false,error:'Thumbnail write failed'}});
    return route.fulfill({json:{ok:true}});
  });
  await page.route(/\/api\/(wifi|mqtt)$/, route => {
    calls.saves.push(new URLSearchParams(route.request().postData()));
    return route.fulfill(options.saveError ? {status:500,json:{ok:false,error:'Could not persist settings'}} : {json:{ok:true}});
  });
  await page.route('**/show?*', route => {
    const name = new URL(route.request().url()).searchParams.get('file'); calls.shows.push(name);
    status.active = {id:1,name:'first.jpg'}; status.pending = {id:2,name}; status.stage = 'refreshing';
    return route.fulfill({status:202,json:{ok:true,job_id:2}});
  });
  return {calls, status};
}
async function jpeg(page, name='test.jpg') {
  const encoded = await page.evaluate(() => {
    const c = document.createElement('canvas'); c.width=1200;c.height=1600;
    const x=c.getContext('2d');x.fillStyle='#d65b48';x.fillRect(0,0,600,1600);x.fillStyle='#48a0d6';x.fillRect(600,0,600,1600);
    return c.toDataURL('image/jpeg').split(',')[1];
  });
  return {name,mimeType:'image/jpeg',buffer:Buffer.from(encoded,'base64')};
}
test('large gallery stays bounded, paginates, and treats filenames as text', async ({page}) => {
  await mockFrame(page,{total:1000,hostileName:true}); await page.goto('/');
  await expect(page.locator('#gallery .card')).toHaveCount(24);
  await expect(page.locator('#gallery .card-name').first()).toHaveText('<img src=x onerror=alert(1)>.jpg');
  await expect(page.locator('#gallery .card-name img')).toHaveCount(0);
  await page.getByRole('button',{name:'Next',exact:true}).click();
  await expect(page.locator('#page-number')).toHaveText('Page 2');
  await expect(page.locator('#gallery .card')).toHaveCount(24);
  await page.getByRole('button',{name:'Previous',exact:true}).click();
  await expect(page.locator('#page-number')).toHaveText('Page 1');
  expect(await page.evaluate(()=>document.documentElement.scrollWidth <= innerWidth)).toBeTruthy();
});
test('optional crop supports rotation, zoom, drag, and frame-sized conversion', async ({page}) => {
  await mockFrame(page); await page.goto('/');
  await page.locator('#files').setInputFiles(await jpeg(page));
  await page.getByRole('button',{name:'Edit crop'}).click();
  await page.getByRole('button',{name:'Rotate 90°'}).click();
  await page.locator('#zoom').fill('2');
  const box = await page.locator('#crop-canvas').boundingBox();
  await page.mouse.move(box.x+box.width*.5,box.y+box.height*.5);await page.mouse.down();
  await page.mouse.move(box.x+box.width*.7,box.y+box.height*.5);await page.mouse.up();
  await page.getByRole('button',{name:'Use this crop'}).click();
  await expect(page.locator('#uploads')).toContainText('Crop ready');
  const size = await page.evaluate(async()=>{
    const f = document.getElementById('files').files[0];const result = await preparePhoto(f,{rotation:90,zoom:2,x:.5,y:.5});
    const image = await createImageBitmap(result.photo);const thumb = await createImageBitmap(result.thumbnail);
    const dimensions=[image.width,image.height,thumb.width,thumb.height];image.close();thumb.close();return dimensions;
  });
  expect(size).toEqual([800,480,400,240]);
  await page.locator('#upload').click();await expect(page.locator('#notice')).toContainText('1 of 1 photos uploaded successfully');
});
test('thumbnail retry does not upload the stored photo again', async ({page}) => {
  const {calls} = await mockFrame(page,{thumbnailError:true});await page.goto('/');
  await page.locator('#files').setInputFiles(await jpeg(page));await page.locator('#upload').click();
  await expect(page.locator('#uploads')).toContainText('Photo saved; thumbnail failed');
  await page.getByRole('button',{name:'Retry',exact:true}).click();await expect(page.locator('#uploads')).toContainText('Uploaded');
  expect(calls.uploads).toBe(1);expect(calls.thumbs).toBe(2);
});
test('mixed upload results stay accurate and failed photos can retry', async ({page}) => {
  const {calls} = await mockFrame(page,{uploadError:true});await page.goto('/');
  await page.locator('#files').setInputFiles([await jpeg(page,'one.jpg'),await jpeg(page,'two.jpg')]);
  await page.locator('#upload').click();await expect(page.locator('#notice')).toContainText('1 of 2 photos uploaded successfully');
  await expect(page.locator('#uploads')).toContainText('SD card is full');
  await page.getByRole('button',{name:'Retry',exact:true}).first().click();
  await expect(page.locator('#uploads .card p').filter({hasText:/^Uploaded$/})).toHaveCount(2);
  expect(calls.uploads).toBe(3);
});
test('password preservation, explicit clearing, and failed persistence are visible', async ({page}) => {
  const {calls} = await mockFrame(page);await page.goto('/wifi');
  await expect(page.locator('#password')).toHaveValue('');
  await page.getByRole('button',{name:'Save settings'}).click();
  await expect(page.locator('#notice')).toContainText('Saved.');
  expect(calls.saves[0].get('pass')).toBe('');expect(calls.saves[0].get('clear_password')).toBe('false');
  await page.locator('#clear-password').check();await page.getByRole('button',{name:'Save settings'}).click();
  await expect.poll(()=>calls.saves.length).toBe(2);expect(calls.saves[1].get('clear_password')).toBe('true');
  await page.route('**/api/wifi', route=>route.fulfill({status:500,json:{error:'Could not persist settings'}}));
  await page.getByRole('button',{name:'Save settings'}).click();await expect(page.locator('#notice')).toContainText('Could not persist');
});
test('Show reports pending work while the frame is refreshing', async ({page}) => {
  await mockFrame(page);await page.goto('/');await page.getByRole('button',{name:'Show',exact:true}).click();
  await expect(page.locator('#device')).toContainText('Display: refreshing');
  await expect(page.locator('#device')).toContainText('Next requested: photo_0.jpg');
});
test('a lost upload response retries the same token', async ({page}) => {
  const {calls} = await mockFrame(page,{lostResponse:true});await page.goto('/');
  await page.locator('#files').setInputFiles(await jpeg(page));await page.locator('#upload').click();
  await expect(page.locator('#uploads')).toContainText('Connection lost');
  await page.getByRole('button',{name:'Retry',exact:true}).click();
  await expect(page.locator('#notice')).toContainText('1 of 1 photos uploaded successfully');
  expect(calls.tokens).toHaveLength(2);expect(calls.tokens[0]).toMatch(/^[0-9a-f]{32}$/);
  expect(calls.tokens[1]).toBe(calls.tokens[0]);
});
test('empty gallery, stale cursor, and invalid conversion recover cleanly', async ({page}) => {
  await mockFrame(page,{total:25});await page.goto('/');
  await expect(page.locator('#gallery .card')).toHaveCount(24);
  await page.route('**/api/photos?*', route => {
    const cursor = new URL(route.request().url()).searchParams.get('cursor');
    return route.fulfill(cursor ? {status:409,json:{restart:true}} : {json:{photos:[],next_cursor:null,generation:2}});
  });
  await page.getByRole('button',{name:'Next',exact:true}).click();
  await expect(page.locator('#page-number')).toHaveText('Page 1');
  await expect(page.locator('#gallery')).toContainText('No photos yet');
  await page.locator('#files').setInputFiles({name:'broken.jpg',mimeType:'image/jpeg',buffer:Buffer.from('not an image')});
  await expect(page.locator('#uploads')).toContainText('Cannot convert');
  await page.locator('#upload').click();
  await expect(page.locator('#notice')).toContainText('0 of 1 photos uploaded successfully');
});
