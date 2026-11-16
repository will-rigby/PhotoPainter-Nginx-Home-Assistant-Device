/* SPDX-License-Identifier: 0BSD */
'use strict';

function cropRect(width, height, crop) {
  const rotated = crop.rotation % 180 !== 0;
  const rw = rotated ? height : width, rh = rotated ? width : height;
  const scale = Math.max(800 / rw, 480 / rh) * crop.zoom;
  const w = 800 / scale, h = 480 / scale;
  const x = Math.min(rw - w, Math.max(0, crop.x * rw - w / 2));
  const y = Math.min(rh - h, Math.max(0, crop.y * rh - h / 2));
  return {x, y, w, h, rw, rh};
}
const defaultCrop = () => ({x: .5, y: .5, zoom: 1, rotation: 0});
function drawCrop(canvas, bitmap, crop) {
  const r = cropRect(bitmap.width, bitmap.height, crop);
  const ctx = canvas.getContext('2d');
  ctx.save(); ctx.clearRect(0, 0, canvas.width, canvas.height);
  ctx.scale(canvas.width / r.w, canvas.height / r.h); ctx.translate(-r.x, -r.y);
  if (crop.rotation === 90) { ctx.translate(bitmap.height, 0); ctx.rotate(Math.PI / 2); }
  if (crop.rotation === 180) { ctx.translate(bitmap.width, bitmap.height); ctx.rotate(Math.PI); }
  if (crop.rotation === 270) { ctx.translate(0, bitmap.width); ctx.rotate(-Math.PI / 2); }
  ctx.drawImage(bitmap, 0, 0); ctx.restore();
  return r;
}
function canvasBlob(canvas, quality) {
  return new Promise((resolve, reject) => canvas.toBlob(blob => blob ? resolve(blob) : reject(new Error('Image conversion failed')), 'image/jpeg', quality));
}
async function preparePhoto(file, crop, previewOnly = false) {
  const bitmap = await createImageBitmap(file, {imageOrientation: 'from-image'});
  try {
    const canvas = document.createElement('canvas'); canvas.width = 800; canvas.height = 480;
    drawCrop(canvas, bitmap, crop);
    const thumb = document.createElement('canvas'); thumb.width = 400; thumb.height = 240;
    thumb.getContext('2d').drawImage(canvas, 0, 0, 400, 240);
    const thumbnail = await canvasBlob(thumb, .8);
    if (previewOnly) return {thumbnail};
    const blob = await canvasBlob(canvas, .9);
    return {photo: new File([blob], file.name.replace(/\.[^.]*$/, '') + '.jpg', {type: 'image/jpeg'}), thumbnail};
  } finally { bitmap.close(); }
}
function uploadToken() {
  return [...crypto.getRandomValues(new Uint8Array(16))].map(n => n.toString(16).padStart(2, '0')).join('');
}
async function api(url, options = {}) {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), 15000);
  try {
    const response = await fetch(url, {...options, signal: controller.signal, cache: 'no-store'});
    const body = await response.json();
    if (!response.ok) { const error = new Error(body.error || `Request failed (${response.status})`); error.status = response.status; error.restart = body.restart; throw error; }
    return body;
  } finally { clearTimeout(timer); }
}
function uploadRequest(url, blob, name, onProgress) {
  return new Promise((resolve, reject) => {
    const request = new XMLHttpRequest(); request.open('POST', url); request.timeout = 120000;
    request.upload.onprogress = e => { if (e.lengthComputable) onProgress(Math.round(e.loaded / e.total * 100)); };
    request.onload = () => {
      try { const result = JSON.parse(request.responseText); if (request.status < 200 || request.status >= 300 || !result.ok) throw new Error(result.error || 'Upload failed'); resolve(result); }
      catch (error) { reject(error); }
    };
    request.onerror = () => reject(new Error('Connection lost. Retry safely when the frame is reachable.'));
    request.ontimeout = () => reject(new Error('Upload timed out. Retry safely.'));
    const form = new FormData(); form.append('file', blob, name); request.send(form);
  });
}

async function startApp() {
  const $ = id => document.getElementById(id);
  const el = (tag, text, className) => { const e = document.createElement(tag); if (text !== undefined) e.textContent = text; if (className) e.className = className; return e; };
  const button = (text, action, className) => { const b = el('button', text, className); b.type = 'button'; b.addEventListener('click', action); return b; };
  const notice = (text, bad = false) => { $('notice').textContent = text; $('notice').className = bad ? 'danger' : 'success'; };
  const run = action => async () => { try { await action(); } catch (error) { notice(error.message, true); } };
  const page = document.body.dataset.page;

  if (page !== 'gallery') {
    const form = $('settings');
    const load = async () => {
      const c = await api('/api/config');
      if (page === 'wifi') { $('ssid').value = c.wifi_ssid; $('hostname').value = c.hostname; $('local-address').textContent = `http://${c.hostname}.local`; }
      else { $('host').value = c.mqtt_host; $('port').value = c.mqtt_port; $('user').value = c.mqtt_user; }
      $('password-state').textContent = c[page === 'wifi' ? 'wifi_password_saved' : 'mqtt_password_saved'] ? 'A password is saved. Leave the field blank to keep it.' : 'No password is saved.';
    };
    $('clear-password').onchange = () => { $('password').disabled = $('clear-password').checked; if ($('clear-password').checked) $('password').value = ''; };
    if (page === 'wifi') $('hostname').oninput = () => { $('local-address').textContent = `http://${$('hostname').value}.local`; };
    form.onsubmit = async event => {
      event.preventDefault(); const submit = form.querySelector('button'); submit.disabled = true;
      try {
        const body = new URLSearchParams(new FormData(form)); body.set('clear_password', String($('clear-password').checked));
        await api(`/api/${page}`, {method: 'POST', body});
        $('password').value = ''; $('clear-password').checked = false; $('password').disabled = false;
        notice(page === 'wifi' ? 'Saved. Restart the frame to apply WiFi and hostname changes.' : 'Saved. MQTT will reconnect when the frame finishes its current work.');
        await load();
      } catch (error) { notice(error.message, true); } finally { submit.disabled = false; }
    };
    await load(); return;
  }

  let cursors = [''], pageIndex = 0, nextCursor = null, loadingGallery = false;
  async function loadGallery(reset = false) {
    if (loadingGallery) return;
    if (reset) { cursors = ['']; pageIndex = 0; }
    loadingGallery = true;
    try {
      let data;
      try { data = await api('/api/photos?limit=24&cursor=' + encodeURIComponent(cursors[pageIndex])); }
      catch (error) {
        if (!error.restart) throw error;
        cursors = ['']; pageIndex = 0; data = await api('/api/photos?limit=24');
      }
      const grid = $('gallery'); grid.replaceChildren(); nextCursor = data.next_cursor;
      if (!data.photos.length) grid.append(el('p', 'No photos yet. Choose some above.'));
      for (const photo of data.photos) {
        const card = el('article', undefined, 'card');
        const image = el('img'); image.loading = 'lazy'; image.alt = photo.name;
        const query = `file=${encodeURIComponent(photo.name)}`;
        image.src = `/thumb?${query}&v=${photo.thumbnail_revision}`;
        image.onerror = () => { image.onerror = null; image.src = `/original?${query}&v=${photo.revision}`; };
        const body = el('div', undefined, 'card-body'); body.append(el('p', photo.name, 'card-name'));
        body.append(el('p', photo.failed ? 'Processing failed — Show retries' : photo.cached ? 'Ready to display' : 'Prepared when first displayed', photo.failed ? 'danger' : ''));
        const actions = el('div', undefined, 'card-actions');
        actions.append(button('Show', run(async () => { await api(`/show?${query}`, {method: 'POST'}); notice('Requested. If the panel is busy, this photo will be shown next.'); await pollStatus(); })));
        actions.append(button('Delete', run(async () => { if (!confirm(`Delete ${photo.name}?`)) return; await api(`/delete?${query}`, {method: 'POST'}); notice('Photo deleted.'); await loadGallery(true); }), 'danger'));
        body.append(actions); card.append(image, body); grid.append(card);
      }
      $('page-number').textContent = `Page ${pageIndex + 1}`;
      $('previous').disabled = pageIndex === 0; $('next').disabled = !nextCursor;
    } finally { loadingGallery = false; }
  }
  $('previous').onclick = run(async () => { if (!loadingGallery && pageIndex) { --pageIndex; await loadGallery(); } });
  $('next').onclick = run(async () => { if (!loadingGallery && nextCursor) { cursors[++pageIndex] = nextCursor; await loadGallery(); } });
  $('refresh').onclick = run(() => loadGallery(true));
  $('clear-cache').onclick = run(async () => { if (confirm('Clear cached panel images? They will be rebuilt when displayed.')) { await api('/redither', {method: 'POST'}); notice('Cache clearing requested.'); } });
  $('test-pattern').onclick = run(async () => { await api('/test', {method: 'POST'}); notice('Test pattern requested.'); });

  let polling = false, lastJob = 0, pollTimer;
  async function pollStatus() {
    if (polling || document.hidden) return;
    polling = true;
    try {
      const s = await api('/api/status'); const device = $('device'); device.replaceChildren();
      const line = (text, className) => device.append(el('p', text, className));
      line(`Display: ${s.stage}${s.active?.name ? ' · ' + s.active.name : ''}`);
      line(`On frame: ${s.displayed || 'Not yet known'}`);
      if (s.pending) line(`Next requested: ${s.pending.name || 'Photo rotation'}`, 'wide');
      line(`Next rotation: ${Math.ceil(s.next_rotation_seconds / 60)} min`);
      const age = at => Math.max(0, Math.floor((s.now - at) / 1000));
      line(s.sd_ready ? (s.storage_at ? `SD: ${(s.storage_free / 1073741824).toFixed(2)} GB free / ${(s.storage_total / 1073741824).toFixed(2)} GB (${age(s.storage_at)}s ago)` : 'SD: checking capacity…') : 'SD card unavailable');
      line(`${s.ap_mode ? 'Setup hotspot' : 'WiFi: ' + s.rssi + ' dBm'} · ${s.ip}`);
      if (!s.ap_mode) line(`http://${s.hostname}.local`);
      line(s.mqtt_at ? `MQTT: ${s.mqtt_connected ? 'connected' : 'disconnected'} (${age(s.mqtt_at)}s ago)` : 'MQTT: not checked yet');
      if (s.last_error) line(`Last error: ${s.last_error}`, 'wide danger');
      if (s.stopping) line('USB removed. Finishing work before sleeping.', 'wide');
      $('test-pattern').disabled = !!(s.active || s.pending || s.stopping);
      $('clear-cache').disabled = !!(s.active || s.pending || s.stopping);
      if (lastJob !== s.last_job) { lastJob = s.last_job; await loadGallery(); }
    } catch (error) { $('device').replaceChildren(el('p', 'Frame unavailable. Retrying…', 'danger')); }
    finally { polling = false; }
  }
  async function pollLoop() { await pollStatus(); if (!document.hidden) pollTimer = setTimeout(pollLoop, 2000); }
  document.addEventListener('visibilitychange', () => { clearTimeout(pollTimer); if (!document.hidden) pollLoop(); });

  let items = [], uploading = false, selecting = false;
  function updateItem(item, text, failed = false) { item.label.textContent = text; item.label.className = failed ? 'danger' : ''; }
  function setBusy(busy) {
    uploading = busy; $('files').disabled = busy; $('upload').disabled = busy || selecting || !items.some(i => !i.complete);
    for (const item of items) { item.edit.disabled = busy || item.attempted || !item.preview; item.retry.disabled = busy || item.complete; }
  }
  async function updatePreview(item) {
    const {thumbnail} = await preparePhoto(item.file, item.crop, true);
    if (item.preview) URL.revokeObjectURL(item.preview);
    item.preview = URL.createObjectURL(thumbnail); item.image.src = item.preview;
  }
  $('files').onchange = async () => {
    selecting = true; $('files').disabled = true; $('upload').disabled = true;
    for (const item of items) if (item.preview) URL.revokeObjectURL(item.preview);
    items = []; $('uploads').replaceChildren();
    try {
      for (const file of $('files').files) {
        const item = {file, crop: defaultCrop(), token: uploadToken(), complete: false, attempted: false}; items.push(item);
        const card = el('article', undefined, 'card'), body = el('div', undefined, 'card-body');
        item.image = el('img'); item.image.alt = file.name;
        body.append(el('p', file.name, 'card-name')); item.label = el('p', 'Preparing preview…');
        item.progress = el('progress'); item.progress.max = 100; item.progress.value = 0; item.progress.setAttribute('aria-label', `Upload progress for ${file.name}`);
        item.edit = button('Edit crop', run(() => openEditor(item))); item.edit.disabled = true;
        item.retry = button('Retry', run(async () => { setBusy(true); try { await sendItem(item); reportBatch(); await loadGallery(true); } finally { setBusy(false); } })); item.retry.hidden = true;
        const actions = el('div', undefined, 'card-actions'); actions.append(item.edit, item.retry);
        body.append(item.label, item.progress, actions); card.append(item.image, body); $('uploads').append(card);
        try { await updatePreview(item); item.edit.disabled = false; updateItem(item, 'Ready'); }
        catch (error) { updateItem(item, `Cannot convert this image: ${error.message}`, true); item.retry.hidden = false; }
      }
    } finally { selecting = false; $('files').disabled = false; setBusy(false); }
  };
  async function sendItem(item) {
    item.attempted = true; item.edit.disabled = true; item.retry.hidden = true;
    try {
      if (!item.storedName) {
        updateItem(item, 'Converting…');
        const prepared = await preparePhoto(item.file, item.crop); item.thumbnail = prepared.thumbnail;
        updateItem(item, 'Uploading…');
        const result = await uploadRequest('/upload?token=' + item.token, prepared.photo, prepared.photo.name, n => { item.progress.value = n; updateItem(item, `Uploading ${n}%`); });
        item.storedName = result.name;
      }
      updateItem(item, 'Uploading thumbnail…');
      await uploadRequest('/thumb?name=' + encodeURIComponent(item.storedName), item.thumbnail, 'thumbnail.jpg', () => {});
      item.thumbnail = null; item.complete = true; item.progress.value = 100; updateItem(item, 'Uploaded');
    } catch (error) {
      updateItem(item, `${item.storedName ? 'Photo saved; thumbnail failed. ' : ''}${error.message}`, true); item.retry.hidden = false;
    }
  }
  $('upload').onclick = run(async () => {
    setBusy(true);
    try { for (const item of items) if (!item.complete) await sendItem(item);
      reportBatch();
      await loadGallery(true);
    } finally { setBusy(false); }
  });
  function reportBatch() {
    const complete = items.filter(i => i.complete).length;
    notice(`${complete} of ${items.length} photos uploaded successfully.`, complete !== items.length);
  }

  let editItem, editBitmap, edited, drag;
  const editor = $('editor'), canvas = $('crop-canvas');
  function redraw() { if (editBitmap) drawCrop(canvas, editBitmap, edited); }
  async function openEditor(item) {
    editBitmap = await createImageBitmap(item.file, {imageOrientation: 'from-image'});
    editItem = item; edited = {...item.crop}; $('zoom').value = edited.zoom; redraw(); editor.showModal();
  }
  editor.addEventListener('close', () => { if (editBitmap) editBitmap.close(); editBitmap = null; editItem = null; drag = null; });
  $('cancel-crop').onclick = () => editor.close();
  $('zoom').oninput = () => { edited.zoom = Number($('zoom').value); redraw(); };
  $('rotate').onclick = () => { edited.rotation = (edited.rotation + 90) % 360; edited.x = edited.y = .5; redraw(); };
  $('reset-crop').onclick = () => { edited = defaultCrop(); $('zoom').value = 1; redraw(); };
  canvas.onpointerdown = event => {
    if (!editBitmap) return;
    canvas.setPointerCapture(event.pointerId);
    const r = cropRect(editBitmap.width, editBitmap.height, edited);
    drag = {x: event.clientX, y: event.clientY, cx: (r.x + r.w / 2) / r.rw, cy: (r.y + r.h / 2) / r.rh, rect: r};
  };
  canvas.onpointermove = event => {
    if (!drag) return;
    const bounds = canvas.getBoundingClientRect(), r = drag.rect;
    edited.x = Math.min(1-r.w/r.rw/2, Math.max(r.w/r.rw/2, drag.cx - (event.clientX - drag.x) / bounds.width * r.w / r.rw));
    edited.y = Math.min(1-r.h/r.rh/2, Math.max(r.h/r.rh/2, drag.cy - (event.clientY - drag.y) / bounds.height * r.h / r.rh)); redraw();
  };
  canvas.onpointerup = canvas.onpointercancel = () => { drag = null; };
  $('save-crop').onclick = run(async () => {
    const item = editItem; item.crop = {...edited}; editor.close(); await updatePreview(item); updateItem(item, 'Crop ready');
  });
  await Promise.all([loadGallery(), pollLoop()]);
}

if (typeof module !== 'undefined') module.exports = {cropRect, defaultCrop, imagePrepare: preparePhoto};
if (typeof document !== 'undefined') startApp().catch(error => { const notice = document.getElementById('notice'); if (notice) { notice.textContent = error.message; notice.className = 'danger'; } });
