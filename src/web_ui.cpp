#include "web_ui.h"
#include "config.h"
#include "counters.h"
#include "display.h"
#include "remote_map.h"
#include "wiz_remote.h"
#include "idle_cycle.h"
#include "show_mode.h"
#include "wifi_mgr.h"

#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>

namespace {
  WebServer server(80);

  const char INDEX_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>Ren Fest Counter</title>
<style>
  :root { --bg:#1a1510; --panel:#2a2218; --ink:#f3e6d0; --muted:#b9a68a; --accent:#c45c26; --line:#4a3c2a; }
  * { box-sizing:border-box; }
  body { margin:0; font-family:Georgia,"Times New Roman",serif; background:linear-gradient(160deg,#1a1510,#2c2116 50%,#1a1510); color:var(--ink); min-height:100vh; }
  main { max-width:820px; margin:0 auto; padding:1.25rem; }
  h1 { font-size:1.75rem; margin:0 0 .25rem; letter-spacing:.02em; }
  .sub { color:var(--muted); margin:0 0 1.25rem; font-size:.95rem; }
  section { background:var(--panel); border:1px solid var(--line); border-radius:8px; padding:1rem 1.1rem; margin-bottom:1rem; }
  h2 { margin:0 0 .75rem; font-size:1.15rem; color:#e8d3b0; }
  label { display:block; font-size:.85rem; color:var(--muted); margin:.4rem 0 .15rem; }
  input[type=text], input[type=number], input[type=password], select { width:100%; padding:.45rem .55rem; border-radius:4px; border:1px solid var(--line); background:#17120c; color:var(--ink); font:inherit; }
  input[type=range] { width:100%; accent-color:var(--accent); }
  .row { display:grid; grid-template-columns:1fr 1fr; gap:.75rem; }
  .row3 { display:grid; grid-template-columns:2fr 1fr auto; gap:.6rem; align-items:end; }
  .counter { border-top:1px solid var(--line); padding-top:.75rem; margin-top:.75rem; }
  .counter:first-of-type { border-top:none; margin-top:0; padding-top:0; }
  .checks { display:flex; gap:1rem; align-items:center; margin-top:.5rem; }
  button, .btn { cursor:pointer; background:var(--accent); color:#fff; border:none; border-radius:4px; padding:.55rem .9rem; font:inherit; }
  button.secondary { background:#3d3224; }
  button:disabled { opacity:.5; cursor:default; }
  .actions { display:flex; flex-wrap:wrap; gap:.5rem; margin-top:1rem; }
  .ok { color:#8fca6b; font-size:.9rem; min-height:1.2em; }
  .mac { font-family:ui-monospace,Consolas,monospace; letter-spacing:.04em; }
  table { width:100%; border-collapse:collapse; font-size:.95rem; }
  th, td { text-align:left; padding:.4rem .3rem; border-bottom:1px solid var(--line); }
  .list-row { display:grid; grid-template-columns:1fr auto; gap:.5rem; align-items:end; margin-bottom:.45rem; }
  .list-row.msg { grid-template-columns:1fr 1fr; align-items:start; }
  .list-row.playlist { grid-template-columns:1fr 1.2fr 1fr 1fr auto; }
  .anim-speeds-drop { margin-top:.35rem; border:1px solid var(--line); border-radius:6px; background:#17120c; }
  .anim-speeds-drop > summary {
    cursor:pointer; list-style:none; padding:.55rem .7rem; color:#e8d3b0; font-size:.9rem;
    display:flex; align-items:center; justify-content:space-between; user-select:none;
  }
  .anim-speeds-drop > summary::-webkit-details-marker { display:none; }
  .anim-speeds-drop > summary::after { content:"▾"; color:var(--muted); font-size:.85rem; transition:transform .15s; }
  .anim-speeds-drop[open] > summary::after { transform:rotate(-180deg); }
  .anim-speeds { display:grid; grid-template-columns:1fr; gap:.55rem; padding:.35rem .7rem .75rem; }
  .anim-speeds label { margin:0; font-size:.8rem; display:flex; justify-content:space-between; gap:.5rem; }
  .anim-speeds input[type=range] { width:100%; margin-top:.15rem; }
  @media (min-width:560px){
    .anim-speeds { grid-template-columns:1fr 1fr; gap:.55rem .9rem; }
  }
  .mini-actions { display:flex; flex-wrap:wrap; gap:.4rem; margin-top:.35rem; }
  .mini-actions button { padding:.3rem .65rem; font-size:.85rem; }
  @media (max-width:600px){ .row,.row3{ grid-template-columns:1fr; } .list-row,.list-row.playlist{ grid-template-columns:1fr; } }
</style>
</head>
<body>
<main>
  <h1>Ren Fest Counter</h1>
  <p class="sub">Configure counters, brightness, Wi‑Fi, and WiZ keys. SoftAP stays on for setup; join your LAN below when you want.</p>

  <section>
    <h2>Global</h2>
    <div class="row">
      <div>
        <label for="intensity">LED intensity (0–15)</label>
        <input id="intensity" type="number" min="0" max="15"/>
      </div>
      <div>
        <label>Display</label>
        <div class="checks">
          <label><input type="checkbox" id="blanked"/> Blanked</label>
        </div>
        <label for="blankPct">Blank brightness % (0 = full off)</label>
        <input id="blankPct" type="number" min="0" max="100" title="0 shuts the MAX7219 off; 1–100 dims to that percent"/>
      </div>
    </div>
    <p>Active counter (remote ±): <strong id="activeLabel">—</strong></p>
  </section>

  <section>
    <h2>Effects</h2>
    <p class="sub" style="margin:0 0 .5rem">Defaults for idle playlist presets and empty-playlist counter cycle. Keymap In/Out override remote keys.</p>
    <div class="row">
      <div>
        <label for="idleEffectIn">Idle in</label>
        <select id="idleEffectIn"></select>
      </div>
      <div>
        <label for="idleEffectOut">Idle out</label>
        <select id="idleEffectOut"></select>
      </div>
    </div>
    <div class="row" style="margin-top:.75rem">
      <div>
        <label for="idleTimeoutSec">Idle start seconds (5–600)</label>
        <input id="idleTimeoutSec" type="number" min="5" max="600"/>
      </div>
      <div>
        <label for="idleCycleSec">Idle cycle seconds (1–120)</label>
        <input id="idleCycleSec" type="number" min="1" max="120"/>
      </div>
    </div>
    <div class="row" style="margin-top:.75rem">
      <div>
        <label for="scrollSpeed">Marquee speed <span id="scrollSpeedVal">5</span> (1–20)</label>
        <input id="scrollSpeed" type="range" min="1" max="20" step="1"/>
      </div>
      <div>
        <label for="teeterSpeed">Teeter speed <span id="teeterSpeedVal">5</span> (1–20)</label>
        <input id="teeterSpeed" type="range" min="1" max="20" step="1"/>
      </div>
    </div>
    <details class="anim-speeds-drop" id="animSpeedsDrop">
      <summary>Anim speeds (1–20)</summary>
      <p class="sub" style="margin:0 .7rem .35rem">Each transition type uses its own speed. Random uses the picked anim's speed. Keymap Speed overrides for that button (0 = use anim default).</p>
      <div class="anim-speeds" id="animSpeeds"></div>
    </details>
  </section>

  <section>
    <h2>Scroller</h2>
    <p class="sub" style="margin:0 0 .5rem">Night / Show cycles all non-empty messages until Stop (or Night again). Scrolling lines loop; short lines hold for Idle cycle seconds between messages.</p>
    <div id="scrollMessages"></div>
    <div class="mini-actions">
      <button type="button" class="secondary" id="btnMsgAdd">+ message</button>
      <button type="button" class="secondary" id="btnMsgRemove">− message</button>
    </div>
    <div class="actions">
      <button type="button" id="btnScrollerShow">Show</button>
      <button type="button" class="secondary" id="btnScrollerStop">Stop</button>
    </div>
  </section>

  <section>
    <h2>Idle order</h2>
    <p class="sub" style="margin:0 0 .5rem">Attract sequence. Use Next message / Next counter to walk lists in order, or pin a specific item. Empty / Counters only = enabled counters. Anim per step (messages: Left/Right).</p>
    <div class="mini-actions" style="margin:0 0 .6rem">
      <button type="button" class="secondary" id="btnPlPresetCounters">Counters only</button>
      <button type="button" class="secondary" id="btnPlPresetMessages">Messages only</button>
      <button type="button" class="secondary" id="btnPlPresetPair">1 msg + 1 counter</button>
    </div>
    <div id="idlePlaylist"></div>
    <div class="mini-actions">
      <button type="button" class="secondary" id="btnPlAdd">+ step</button>
    </div>
  </section>

  <section>
    <h2>Counters</h2>
    <div id="counters"></div>
  </section>

  <section>
    <h2>WiZ remote</h2>
    <p>Last seen: <span class="mac" id="lastMac">—</span></p>
    <p>Linked: <span class="mac" id="linkMac">none (accept any)</span></p>
    <div class="actions">
      <button type="button" id="btnLink">Link last seen</button>
      <button type="button" class="secondary" id="btnUnlink">Clear link</button>
    </div>
    <h2 style="margin-top:1.25rem">Key map</h2>
    <p class="sub" style="margin:0 0 .5rem">Tap fires immediately. In = enter / play; Out = exit when leaving. Speed 0 = anim default (1–20).</p>
    <table>
      <thead><tr><th>Button</th><th>Tap</th><th>Counter</th><th>In</th><th>Out</th><th>Speed</th></tr></thead>
      <tbody id="keymap"></tbody>
    </table>
  </section>

  <section>
    <h2>Firmware update</h2>
    <p class="sub" style="margin:0 0 .75rem">Upload <code>firmware.bin</code> from SoftAP (no USB). Build with <code>pio run</code>, file is under <code>.pio/build/esp32dev/</code>.</p>
    <form method="POST" action="/update" enctype="multipart/form-data">
      <input type="file" name="firmware" accept=".bin"/>
      <div class="actions">
        <button type="submit">Upload &amp; reboot</button>
      </div>
    </form>
  </section>

  <div class="actions">
    <button type="button" id="btnSave">Save</button>
    <button type="button" class="secondary" id="btnReload">Reload</button>
  </div>
  <p class="ok" id="status"></p>

  <section>
    <h2>Wi‑Fi</h2>
    <p class="sub" style="margin:0 0 .5rem">SoftAP <code id="apSsidLabel">RenFest-Counter</code> stays on (config / OTA). Station joins your home network.</p>
    <p>SoftAP: <span class="mac" id="apIp">—</span></p>
    <p>Station: <span id="staStatus">—</span> <span class="mac" id="staIp"></span></p>
    <div class="actions">
      <button type="button" id="btnWifiScan">Scan networks</button>
      <button type="button" class="secondary" id="btnWifiForget">Forget network</button>
    </div>
    <div class="row" style="margin-top:.75rem">
      <div>
        <label for="wifiSsid">Network</label>
        <select id="wifiSsid"><option value="">— scan first —</option></select>
      </div>
      <div>
        <label for="wifiPass">Password</label>
        <input id="wifiPass" type="password" autocomplete="off"/>
      </div>
    </div>
    <label for="wifiSsidManual">Or type SSID</label>
    <input id="wifiSsidManual" type="text" maxlength="32" placeholder="optional if not in list"/>
    <div class="actions">
      <button type="button" id="btnWifiConnect">Connect</button>
    </div>
  </section>
</main>
<script>
const ACTION_OPTS = [
  {v:0,t:'none'},
  {v:1,t:'select'},
  {v:2,t:'inc_active'},
  {v:3,t:'dec_active'},
  {v:4,t:'blank'},
  {v:5,t:'unblank'},
  {v:6,t:'scroller'}
];
const ANIM_OPTS = [
  {v:0,t:'Left'},
  {v:1,t:'Right'},
  {v:2,t:'Up'},
  {v:3,t:'Down'},
  {v:4,t:'None'},
  {v:5,t:'Animation'},
  {v:6,t:'Piling'},
  {v:7,t:'Splite'},
  {v:8,t:'Laser Left'},
  {v:12,t:'Laser Right'},
  {v:13,t:'Laser Up'},
  {v:14,t:'Laser Down'},
  {v:9,t:'Smoth'},
  {v:10,t:'Rotate'},
  {v:11,t:'Random'}
];
// Speed-tunable modes (skip None / Random)
const ANIM_SPEED_OPTS = ANIM_OPTS.filter(o => o.v !== 4 && o.v !== 11);

function animOptions(selected){
  return ANIM_OPTS.map(o=>`<option value="${o.v}" ${o.v===selected?'selected':''}>${o.t}</option>`).join('');
}

function fillAnimSelect(id, selected){
  const s = el(id);
  s.innerHTML = animOptions(selected != null ? selected : 0);
}

let state = null;

function el(id){ return document.getElementById(id); }
function setStatus(m){ el('status').textContent = m || ''; }

async function load(){
  const r = await fetch('/api/state');
  state = await r.json();
  render();
  setStatus('Loaded');
}

function render(){
  el('intensity').value = state.intensity;
  el('blanked').checked = !!state.blanked;
  el('blankPct').value = (state.blankBrightnessPercent != null) ? state.blankBrightnessPercent : 0;
  el('activeLabel').textContent = (state.activeIndex+1) + ' — ' + (state.counters[state.activeIndex]?.name || '');
  el('lastMac').textContent = state.lastSeenMac || '—';
  el('linkMac').textContent = state.linkedMac || 'none (accept any)';
  fillAnimSelect('idleEffectIn', (state.idleEffectIn != null) ? state.idleEffectIn : state.idleEffect);
  fillAnimSelect('idleEffectOut', (state.idleEffectOut != null) ? state.idleEffectOut : state.idleEffect);
  el('idleCycleSec').value = (state.idleCycleSeconds != null) ? state.idleCycleSeconds : 5;
  el('idleTimeoutSec').value = (state.idleTimeoutSeconds != null) ? state.idleTimeoutSeconds : 30;
  const spd = (state.scrollSpeed != null) ? state.scrollSpeed : 5;
  el('scrollSpeed').value = spd;
  el('scrollSpeedVal').textContent = spd;
  const tspd = (state.teeterSpeed != null) ? state.teeterSpeed : 5;
  el('teeterSpeed').value = tspd;
  el('teeterSpeedVal').textContent = tspd;

  if (!state.effectSpeeds || typeof state.effectSpeeds !== 'object') {
    state.effectSpeeds = {};
  }
  const spdBox = el('animSpeeds');
  spdBox.innerHTML = '';
  ANIM_SPEED_OPTS.forEach(o=>{
    const v = (state.effectSpeeds[o.v] != null) ? state.effectSpeeds[o.v]
            : ((state.effectSpeeds[String(o.v)] != null) ? state.effectSpeeds[String(o.v)] : spd);
    const d = document.createElement('div');
    d.innerHTML = `<label for="esp${o.v}"><span>${o.t}</span><span id="espVal${o.v}">${v}</span></label>
      <input id="esp${o.v}" type="range" min="1" max="20" step="1" data-esp="${o.v}" value="${v}"/>`;
    spdBox.appendChild(d);
    const inp = d.querySelector('input');
    const valEl = d.querySelector(`#espVal${o.v}`);
    inp.oninput = ()=>{ valEl.textContent = inp.value; };
  });

  if (!Array.isArray(state.scrollMessages) || !state.scrollMessages.length) {
    state.scrollMessages = [{top: state.scrollMessage || '', bottom: ''}];
  } else {
    state.scrollMessages = state.scrollMessages.map(m=>{
      if (m && typeof m === 'object') {
        return {top: m.top || '', bottom: m.bottom || ''};
      }
      return {top: String(m||''), bottom: ''};
    });
  }
  if (!Array.isArray(state.idlePlaylist)) state.idlePlaylist = [];

  const msgBox = el('scrollMessages');
  msgBox.innerHTML = '';
  state.scrollMessages.forEach((m,i)=>{
    const d = document.createElement('div');
    d.className = 'list-row msg';
    const label = i===0 ? 'Message 1 (remote Show)' : ('Message ' + (i+1));
    d.innerHTML = `
      <div>
        <label>${label} — top (scroll)</label>
        <input type="text" maxlength="64" data-msg="${i}" data-f="top" value="${escapeAttr(m.top||'')}" placeholder="marquee text"/>
      </div>
      <div>
        <label>Bottom (counter row)</label>
        <input type="text" maxlength="16" data-msg="${i}" data-f="bottom" value="${escapeAttr(m.bottom||'')}" placeholder="optional"/>
      </div>`;
    msgBox.appendChild(d);
  });
  el('btnMsgAdd').disabled = state.scrollMessages.length >= 8;
  el('btnMsgRemove').disabled = state.scrollMessages.length <= 1;

  const plBox = el('idlePlaylist');
  plBox.innerHTML = '';
  const defaultIn = (state.idleEffectIn != null) ? state.idleEffectIn : ((state.idleEffect != null) ? state.idleEffect : 0);
  const defaultOut = (state.idleEffectOut != null) ? state.idleEffectOut : defaultIn;
  state.idlePlaylist.forEach((step,i)=>{
    const d = document.createElement('div');
    d.className = 'list-row playlist';
    const kind = (step.kind === 1) ? 1 : 0;
    const idx = (step.index != null) ? step.index : 255;
    const effIn = (step.effectIn != null) ? step.effectIn : ((step.effect != null) ? step.effect : defaultIn);
    const effOut = (step.effectOut != null) ? step.effectOut : ((step.effect2 != null) ? step.effect2 : defaultOut);
    let idxOpts = '';
    if (kind === 1) {
      idxOpts += `<option value="255" ${idx===255?'selected':''}>Next message</option>`;
      for (let m=0; m<state.scrollMessages.length; m++) {
        idxOpts += `<option value="${m}" ${idx===m?'selected':''}>Msg ${m+1}</option>`;
      }
    } else {
      idxOpts += `<option value="255" ${idx===255?'selected':''}>Next counter</option>`;
      for (let c=0; c<state.counters.length; c++) {
        const nm = state.counters[c]?.name || ('CNT'+(c+1));
        idxOpts += `<option value="${c}" ${idx===c?'selected':''}>${c+1} — ${escapeAttr(nm)}</option>`;
      }
    }
    d.innerHTML = `
      <div>
        <label>Type</label>
        <select data-pl="${i}" data-f="kind">
          <option value="0" ${kind===0?'selected':''}>Counter</option>
          <option value="1" ${kind===1?'selected':''}>Message</option>
        </select>
      </div>
      <div>
        <label>Which</label>
        <select data-pl="${i}" data-f="index">${idxOpts}</select>
      </div>
      <div>
        <label>In</label>
        <select data-pl="${i}" data-f="effectIn">${animOptions(effIn)}</select>
      </div>
      <div>
        <label>Out</label>
        <select data-pl="${i}" data-f="effectOut">${animOptions(effOut)}</select>
      </div>
      <div>
        <label>&nbsp;</label>
        <button type="button" class="secondary" data-pl-rm="${i}">−</button>
      </div>`;
    plBox.appendChild(d);
  });
  el('btnPlAdd').disabled = state.idlePlaylist.length >= 16;
  plBox.querySelectorAll('select[data-f=kind]').forEach(s=>{
    s.addEventListener('change', ev=>{
      syncMessagesFromDom();
      syncPlaylistFromDom();
      const i = +ev.target.dataset.pl;
      state.idlePlaylist[i].kind = +ev.target.value;
      state.idlePlaylist[i].index = 255;
      render();
    });
  });
  plBox.querySelectorAll('button[data-pl-rm]').forEach(b=>{
    b.addEventListener('click', ev=>{
      syncMessagesFromDom();
      syncPlaylistFromDom();
      const i = +ev.currentTarget.dataset.plRm;
      state.idlePlaylist.splice(i, 1);
      render();
    });
  });

  const box = el('counters');
  box.innerHTML = '';
  state.counters.forEach((c,i)=>{
    const d = document.createElement('div');
    d.className = 'counter';
    d.innerHTML = `
      <div class="row3">
        <div>
          <label>Counter ${i+1} name</label>
          <input type="text" maxlength="15" data-i="${i}" data-f="name" value="${escapeAttr(c.name)}"/>
        </div>
        <div>
          <label>Count</label>
          <input type="number" min="0" max="9999" data-i="${i}" data-f="count" value="${c.count}"/>
        </div>
        <div>
          <label>&nbsp;</label>
          <label class="checks"><input type="checkbox" data-i="${i}" data-f="enabled" ${c.enabled?'checked':''}/> Enabled</label>
        </div>
      </div>`;
    box.appendChild(d);
  });

  const km = el('keymap');
  km.innerHTML = '';
  state.bindings.forEach(b=>{
    const tr = document.createElement('tr');
    const eIn = (b.effectIn != null) ? b.effectIn : ((b.effect != null) ? b.effect : 0);
    const eOut = (b.effectOut != null) ? b.effectOut : ((b.effect2 != null) ? b.effect2 : 0);
    const spd = (b.speed != null) ? b.speed : 0;
    const selAct = ACTION_OPTS.map(o=>`<option value="${o.v}" ${o.v===b.action?'selected':''}>${o.t}</option>`).join('');
    let selCnt = '';
    for(let i=0;i<state.counters.length;i++){
      selCnt += `<option value="${i}" ${i===b.param?'selected':''}>${i+1}</option>`;
    }
    tr.innerHTML = `
      <td>${b.label} <span class="mac">(${b.buttonId})</span></td>
      <td><select data-btn="${b.buttonId}" data-f="action">${selAct}</select></td>
      <td><select data-btn="${b.buttonId}" data-f="param" ${b.action===1?'':'disabled'}>${selCnt}</select></td>
      <td><select data-btn="${b.buttonId}" data-f="effectIn">${animOptions(eIn)}</select></td>
      <td><select data-btn="${b.buttonId}" data-f="effectOut">${animOptions(eOut)}</select></td>
      <td><input type="number" min="0" max="20" data-btn="${b.buttonId}" data-f="speed" value="${spd}" title="0 = anim default" style="width:3.5rem"/></td>`;
    km.appendChild(tr);
  });

  km.querySelectorAll(`select[data-f=action]`).forEach(s=>{
    s.addEventListener('change', ev=>{
      const btn = +ev.target.dataset.btn;
      const paramSel = km.querySelector(`select[data-btn="${btn}"][data-f=param]`);
      paramSel.disabled = (+ev.target.value !== 1);
    });
  });
}

function escapeAttr(s){
  return String(s).replace(/&/g,'&amp;').replace(/"/g,'&quot;').replace(/</g,'&lt;');
}

function syncMessagesFromDom(){
  if (!state.scrollMessages) state.scrollMessages = [{top:'', bottom:''}];
  state.scrollMessages = state.scrollMessages.map((_,i)=>{
    const topEl = document.querySelector(`input[data-msg="${i}"][data-f=top]`);
    const botEl = document.querySelector(`input[data-msg="${i}"][data-f=bottom]`);
    return {
      top: topEl ? topEl.value : '',
      bottom: botEl ? botEl.value : ''
    };
  });
}

function syncPlaylistFromDom(){
  if (!state.idlePlaylist) state.idlePlaylist = [];
  const defaultIn = (state.idleEffectIn != null) ? state.idleEffectIn : ((state.idleEffect != null) ? state.idleEffect : 0);
  const defaultOut = (state.idleEffectOut != null) ? state.idleEffectOut : defaultIn;
  state.idlePlaylist = state.idlePlaylist.map((step,i)=>{
    const kindEl = document.querySelector(`select[data-pl="${i}"][data-f=kind]`);
    const idxEl = document.querySelector(`select[data-pl="${i}"][data-f=index]`);
    const inEl = document.querySelector(`select[data-pl="${i}"][data-f=effectIn]`);
    const outEl = document.querySelector(`select[data-pl="${i}"][data-f=effectOut]`);
    return {
      kind: kindEl ? +kindEl.value : (step.kind||0),
      index: idxEl ? +idxEl.value : (step.index != null ? step.index : 255),
      effectIn: inEl ? +inEl.value : (step.effectIn != null ? step.effectIn : defaultIn),
      effectOut: outEl ? +outEl.value : (step.effectOut != null ? step.effectOut : defaultOut)
    };
  });
}

function collect(){
  syncMessagesFromDom();
  syncPlaylistFromDom();
  const body = {
    intensity: +el('intensity').value,
    blanked: el('blanked').checked,
    blankBrightnessPercent: +el('blankPct').value,
    idleEffectIn: +el('idleEffectIn').value,
    idleEffectOut: +el('idleEffectOut').value,
    idleCycleSeconds: +el('idleCycleSec').value,
    idleTimeoutSeconds: +el('idleTimeoutSec').value,
    scrollSpeed: +el('scrollSpeed').value,
    teeterSpeed: +el('teeterSpeed').value,
    effectSpeeds: (()=>{
      const o = {};
      document.querySelectorAll('input[data-esp]').forEach(inp=>{
        let v = +inp.value;
        if (v < 1) v = 1;
        if (v > 20) v = 20;
        o[inp.dataset.esp] = v;
      });
      return o;
    })(),
    scrollMessages: state.scrollMessages.slice(),
    idlePlaylist: state.idlePlaylist.slice(),
    counters: state.counters.map((c,i)=>{
      const name = document.querySelector(`input[data-i="${i}"][data-f=name]`).value;
      const count = +document.querySelector(`input[data-i="${i}"][data-f=count]`).value;
      const enEl = document.querySelector(`input[data-i="${i}"][data-f=enabled]`);
      return { enabled: enEl.checked, name, count };
    }),
    bindings: state.bindings.map(b=>{
      const action = +document.querySelector(`select[data-btn="${b.buttonId}"][data-f=action]`).value;
      const param = +document.querySelector(`select[data-btn="${b.buttonId}"][data-f=param]`).value;
      const effectIn = +document.querySelector(`select[data-btn="${b.buttonId}"][data-f=effectIn]`).value;
      const effectOut = +document.querySelector(`select[data-btn="${b.buttonId}"][data-f=effectOut]`).value;
      let speed = +document.querySelector(`input[data-btn="${b.buttonId}"][data-f=speed]`).value;
      if (speed < 0) speed = 0;
      if (speed > 20) speed = 20;
      return { buttonId: b.buttonId, action, param, effectIn, effectOut, speed };
    }),
    linkMac: state.linkedMac || ''
  };
  return body;
}

el('btnSave').onclick = async ()=>{
  setStatus('Saving…');
  try {
    const r = await fetch('/api/state', {
      method:'POST',
      headers:{'Content-Type':'application/json'},
      body: JSON.stringify(collect())
    });
    const text = await r.text();
    if(!r.ok){ setStatus('Save failed: ' + text); return; }
    state = JSON.parse(text);
    render();
    setStatus('Saved');
  } catch(e) {
    setStatus('Save error: ' + e);
  }
};

el('btnReload').onclick = ()=>load();

el('btnMsgAdd').onclick = ()=>{
  syncMessagesFromDom();
  syncPlaylistFromDom();
  if (state.scrollMessages.length >= 8) return;
  state.scrollMessages.push({top:'', bottom:''});
  render();
};
el('btnMsgRemove').onclick = ()=>{
  syncMessagesFromDom();
  syncPlaylistFromDom();
  if (state.scrollMessages.length <= 1) return;
  state.scrollMessages.pop();
  render();
};
el('btnPlAdd').onclick = ()=>{
  syncMessagesFromDom();
  syncPlaylistFromDom();
  if (state.idlePlaylist.length >= 16) return;
  const effIn = (state.idleEffectIn != null) ? state.idleEffectIn : ((state.idleEffect != null) ? state.idleEffect : 0);
  const effOut = (state.idleEffectOut != null) ? state.idleEffectOut : effIn;
  state.idlePlaylist.push({kind:0, index:255, effectIn:effIn, effectOut:effOut});
  render();
};

function applyIdlePreset(builder){
  syncMessagesFromDom();
  syncPlaylistFromDom();
  state.idlePlaylist = builder();
  render();
}

el('btnPlPresetCounters').onclick = ()=>{
  applyIdlePreset(()=>[]);
};
el('btnPlPresetMessages').onclick = ()=>{
  applyIdlePreset(()=>{
    const effIn = (state.idleEffectIn != null) ? state.idleEffectIn : ((state.idleEffect != null) ? state.idleEffect : 0);
    const effOut = (state.idleEffectOut != null) ? state.idleEffectOut : effIn;
    return [{kind:1, index:255, effectIn:effIn, effectOut:effOut}];
  });
};
el('btnPlPresetPair').onclick = ()=>{
  applyIdlePreset(()=>{
    const effIn = (state.idleEffectIn != null) ? state.idleEffectIn : ((state.idleEffect != null) ? state.idleEffect : 0);
    const effOut = (state.idleEffectOut != null) ? state.idleEffectOut : effIn;
    return [
      {kind:1, index:255, effectIn:effIn, effectOut:effOut},
      {kind:0, index:255, effectIn:effIn, effectOut:effOut}
    ];
  });
};

el('scrollSpeed').oninput = ()=>{
  el('scrollSpeedVal').textContent = el('scrollSpeed').value;
};
el('teeterSpeed').oninput = ()=>{
  el('teeterSpeedVal').textContent = el('teeterSpeed').value;
};

el('btnScrollerShow').onclick = async ()=>{
  // Persist message first so device uses latest text
  await el('btnSave').onclick();
  const r = await fetch('/api/scroller', {
    method:'POST', headers:{'Content-Type':'application/json'},
    body: JSON.stringify({action:'start'})
  });
  setStatus(r.ok ? 'Scroller started' : 'Scroller start failed');
};
el('btnScrollerStop').onclick = async ()=>{
  const r = await fetch('/api/scroller', {
    method:'POST', headers:{'Content-Type':'application/json'},
    body: JSON.stringify({action:'stop'})
  });
  setStatus(r.ok ? 'Scroller stopped' : 'Scroller stop failed');
};

el('btnLink').onclick = async ()=>{
  const r = await fetch('/api/link', { method:'POST', headers:{'Content-Type':'application/json'}, body: JSON.stringify({action:'link'}) });
  state = await r.json();
  render();
  setStatus(state.linkedMac ? 'Remote linked' : 'No remote seen yet');
};

el('btnUnlink').onclick = async ()=>{
  const r = await fetch('/api/link', { method:'POST', headers:{'Content-Type':'application/json'}, body: JSON.stringify({action:'clear'}) });
  state = await r.json();
  render();
  setStatus('Link cleared');
};

async function refreshWifi(){
  try {
    const r = await fetch('/api/wifi/status');
    const w = await r.json();
    el('apIp').textContent = w.apIp || '—';
    el('apSsidLabel').textContent = w.apSsid || 'RenFest-Counter';
    let st = w.status || '—';
    if (w.configured && w.ssid) st = w.ssid + ' — ' + st;
    if (w.rssi) st += ' (' + w.rssi + ' dBm)';
    el('staStatus').textContent = st;
    el('staIp').textContent = w.staIp ? ('· ' + w.staIp) : '';
  } catch(e){}
}

el('btnWifiScan').onclick = async ()=>{
  setStatus('Scanning…');
  el('btnWifiScan').disabled = true;
  try {
    const r = await fetch('/api/wifi/scan');
    const w = await r.json();
    const sel = el('wifiSsid');
    sel.innerHTML = '';
    if (!w.networks || !w.networks.length) {
      sel.innerHTML = '<option value="">(none found)</option>';
      setStatus('No networks found');
    } else {
      w.networks.forEach(n=>{
        const o = document.createElement('option');
        o.value = n.ssid;
        o.textContent = n.ssid + '  ' + n.rssi + 'dBm' + (n.secure ? ' *' : '');
        sel.appendChild(o);
      });
      setStatus('Found ' + w.networks.length + ' networks');
    }
  } catch(e) {
    setStatus('Scan failed: ' + e);
  }
  el('btnWifiScan').disabled = false;
  refreshWifi();
};

el('btnWifiConnect').onclick = async ()=>{
  const ssid = (el('wifiSsidManual').value || el('wifiSsid').value || '').trim();
  const pass = el('wifiPass').value;
  if (!ssid) { setStatus('Pick or type an SSID'); return; }
  setStatus('Connecting to ' + ssid + '…');
  try {
    const r = await fetch('/api/wifi/connect', {
      method:'POST',
      headers:{'Content-Type':'application/json'},
      body: JSON.stringify({ssid, password: pass})
    });
    const text = await r.text();
    if (!r.ok) { setStatus('Connect failed: ' + text); return; }
    setStatus('Joining ' + ssid + '… (may take a few seconds)');
    setTimeout(refreshWifi, 2000);
    setTimeout(refreshWifi, 5000);
    setTimeout(refreshWifi, 10000);
  } catch(e) {
    setStatus('Connect error: ' + e);
  }
};

el('btnWifiForget').onclick = async ()=>{
  await fetch('/api/wifi/clear', { method:'POST', headers:{'Content-Type':'application/json'}, body:'{}' });
  el('wifiPass').value = '';
  setStatus('Station network forgotten');
  refreshWifi();
};

load();
refreshWifi();
setInterval(refreshWifi, 5000);
setInterval(async ()=>{
  try {
    const r = await fetch('/api/state');
    const s = await r.json();
    if(s.lastSeenMac !== state.lastSeenMac || s.linkedMac !== state.linkedMac || s.activeIndex !== state.activeIndex){
      state.lastSeenMac = s.lastSeenMac;
      state.linkedMac = s.linkedMac;
      state.activeIndex = s.activeIndex;
      el('lastMac').textContent = state.lastSeenMac || '—';
      el('linkMac').textContent = state.linkedMac || 'none (accept any)';
      el('activeLabel').textContent = (state.activeIndex+1) + ' — ' + (state.counters[state.activeIndex]?.name || '');
    }
  } catch(e){}
}, 2000);
</script>
</body>
</html>
)HTML";

  void sendStateJson() {
    JsonDocument doc;
    doc["intensity"] = Counters::intensity();
    doc["blanked"] = Counters::blanked();
    doc["blankBrightnessPercent"] = Counters::blankBrightnessPercent();
    doc["activeIndex"] = Counters::activeIndex();
    doc["linkedMac"] = Counters::linkedRemoteMac();
    doc["lastSeenMac"] = WizRemote::lastSeenMac();
    doc["idleEffectIn"] = Counters::idleEffectIn();
    doc["idleEffectOut"] = Counters::idleEffectOut();
    doc["idleEffect"] = Counters::idleEffectIn();  // legacy alias
    doc["idleCycleSeconds"] = Counters::idleCycleSeconds();
    doc["idleTimeoutSeconds"] = Counters::idleTimeoutSeconds();
    doc["scrollSpeed"] = Counters::scrollSpeed();
    doc["teeterSpeed"] = Counters::teeterSpeed();
    {
      JsonObject esp = doc["effectSpeeds"].to<JsonObject>();
      for (uint8_t i = 0; i < ShowMode::COUNT; i++) {
        if (i == ShowMode::FREEZE || i == ShowMode::RANDOM) continue;
        char key[4];
        snprintf(key, sizeof(key), "%u", i);
        esp[key] = Counters::effectSpeed(i);
      }
    }
    doc["scrollMessage"] = Counters::scrollMessage();  // legacy alias = message 0 top

    JsonArray msgs = doc["scrollMessages"].to<JsonArray>();
    for (uint8_t i = 0; i < Counters::scrollMessageCount(); i++) {
      JsonObject o = msgs.add<JsonObject>();
      o["top"] = Counters::scrollMessage(i);
      o["bottom"] = Counters::scrollMessageBottom(i);
    }

    JsonArray playlist = doc["idlePlaylist"].to<JsonArray>();
    for (uint8_t i = 0; i < Counters::idlePlaylistLen(); i++) {
      IdleStep step = Counters::idleStep(i);
      JsonObject o = playlist.add<JsonObject>();
      o["kind"] = step.kind;
      o["index"] = step.index;
      o["effectIn"] = step.effectIn;
      o["effectOut"] = step.effectOut;
    }

    JsonArray arr = doc["counters"].to<JsonArray>();
    for (uint8_t i = 0; i < MAX_COUNTERS; i++) {
      const Counter& c = Counters::getConst(i);
      JsonObject o = arr.add<JsonObject>();
      o["enabled"] = c.enabled;
      o["name"] = c.name;
      o["count"] = c.count;
      o["csOk"] = SINGLE_DISPLAY ? true : (CS_PINS[i] >= 0);
    }

    JsonArray binds = doc["bindings"].to<JsonArray>();
    for (uint8_t i = 0; i < RemoteMap::bindingCount(); i++) {
      RemoteMap::Binding b = RemoteMap::getBinding(i);
      JsonObject o = binds.add<JsonObject>();
      o["buttonId"] = b.buttonId;
      o["action"] = b.action;
      o["param"] = b.param;
      o["effectIn"] = b.effectIn;
      o["effectOut"] = b.effectOut;
      o["speed"] = b.speed;
      o["label"] = RemoteMap::buttonLabel(b.buttonId);
    }

    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
  }

  void handleRoot() {
    server.send_P(200, "text/html", INDEX_HTML);
  }

  void handleGetState() { sendStateJson(); }

  // ArduinoJson 7 stores JSON numbers as long/double — don't use .is<int>().
  void applyStateDoc(JsonDocument& doc) {
    if (!doc["intensity"].isNull()) {
      Counters::setIntensity(doc["intensity"].as<uint8_t>());
    }
    if (!doc["blanked"].isNull()) {
      Counters::setBlanked(doc["blanked"].as<bool>());
    }
    if (!doc["blankBrightnessPercent"].isNull()) {
      Counters::setBlankBrightnessPercent(doc["blankBrightnessPercent"].as<uint8_t>());
    }
    if (!doc["idleEffectIn"].isNull()) {
      Counters::setIdleEffectIn(doc["idleEffectIn"].as<uint8_t>());
    } else if (!doc["idleEffect"].isNull()) {
      Counters::setIdleEffectIn(doc["idleEffect"].as<uint8_t>());
    }
    if (!doc["idleEffectOut"].isNull()) {
      Counters::setIdleEffectOut(doc["idleEffectOut"].as<uint8_t>());
    }
    if (!doc["idleCycleSeconds"].isNull()) {
      Counters::setIdleCycleSeconds(doc["idleCycleSeconds"].as<uint8_t>());
    }
    if (!doc["idleTimeoutSeconds"].isNull()) {
      Counters::setIdleTimeoutSeconds(doc["idleTimeoutSeconds"].as<uint16_t>());
    }
    if (!doc["scrollSpeed"].isNull()) {
      Counters::setScrollSpeed(doc["scrollSpeed"].as<uint8_t>());
    }
    if (!doc["teeterSpeed"].isNull()) {
      Counters::setTeeterSpeed(doc["teeterSpeed"].as<uint8_t>());
    }
    {
      JsonObject esp = doc["effectSpeeds"].as<JsonObject>();
      if (!esp.isNull()) {
        for (JsonPair kv : esp) {
          uint8_t mode = static_cast<uint8_t>(atoi(kv.key().c_str()));
          Counters::setEffectSpeed(mode, kv.value().as<uint8_t>());
        }
      }
    }

    JsonArray msgs = doc["scrollMessages"].as<JsonArray>();
    if (!msgs.isNull()) {
      char tops[MAX_SCROLL_MESSAGES][SCROLLER_MAX_LEN + 1];
      char bots[MAX_SCROLL_MESSAGES][SCROLLER_BOTTOM_MAX_LEN + 1];
      const char* topPtrs[MAX_SCROLL_MESSAGES];
      const char* botPtrs[MAX_SCROLL_MESSAGES];
      uint8_t n = 0;
      for (JsonVariant v : msgs) {
        if (n >= MAX_SCROLL_MESSAGES) break;
        tops[n][0] = '\0';
        bots[n][0] = '\0';
        if (v.is<JsonObject>()) {
          JsonObject o = v.as<JsonObject>();
          const char* t = o["top"].as<const char*>();
          const char* b = o["bottom"].as<const char*>();
          if (t) {
            strncpy(tops[n], t, SCROLLER_MAX_LEN);
            tops[n][SCROLLER_MAX_LEN] = '\0';
          }
          if (b) {
            strncpy(bots[n], b, SCROLLER_BOTTOM_MAX_LEN);
            bots[n][SCROLLER_BOTTOM_MAX_LEN] = '\0';
          }
        } else {
          const char* s = v.as<const char*>();
          if (s) {
            strncpy(tops[n], s, SCROLLER_MAX_LEN);
            tops[n][SCROLLER_MAX_LEN] = '\0';
          }
        }
        topPtrs[n] = tops[n];
        botPtrs[n] = bots[n];
        n++;
      }
      if (n == 0) {
        tops[0][0] = '\0';
        bots[0][0] = '\0';
        topPtrs[0] = tops[0];
        botPtrs[0] = bots[0];
        n = 1;
      }
      Counters::setScrollMessages(topPtrs, botPtrs, n);
    } else if (!doc["scrollMessage"].isNull()) {
      const char* msg = doc["scrollMessage"].as<const char*>();
      Counters::setScrollMessage(msg ? msg : "");
    }

    JsonArray playlist = doc["idlePlaylist"].as<JsonArray>();
    if (!playlist.isNull()) {
      IdleStep steps[MAX_IDLE_PLAYLIST];
      uint8_t n = 0;
      for (JsonObject o : playlist) {
        if (n >= MAX_IDLE_PLAYLIST) break;
        IdleStep step;
        step.kind = o["kind"].isNull() ? IdleCounter : o["kind"].as<uint8_t>();
        step.index = o["index"].isNull() ? IdleAutoIndex : o["index"].as<uint8_t>();
        if (!o["effectIn"].isNull()) {
          step.effectIn = o["effectIn"].as<uint8_t>();
        } else if (!o["effect"].isNull()) {
          step.effectIn = o["effect"].as<uint8_t>();
        } else {
          step.effectIn = Counters::idleEffectIn();
        }
        if (!o["effectOut"].isNull()) {
          step.effectOut = o["effectOut"].as<uint8_t>();
        } else if (!o["effect2"].isNull()) {
          step.effectOut = o["effect2"].as<uint8_t>();
        } else {
          step.effectOut = Counters::idleEffectOut();
        }
        steps[n++] = step;
      }
      Counters::setIdlePlaylist(steps, n);
    }

    JsonArray arr = doc["counters"].as<JsonArray>();
    if (!arr.isNull()) {
      uint8_t i = 0;
      for (JsonObject o : arr) {
        if (i >= MAX_COUNTERS) break;
        if (!o["enabled"].isNull()) {
          Counters::setEnabled(i, o["enabled"].as<bool>());
        }
        if (!o["name"].isNull()) {
          const char* name = o["name"].as<const char*>();
          if (name) Counters::setName(i, name);
        }
        if (!o["count"].isNull()) {
          Counters::setCount(i, o["count"].as<int32_t>());
        }
        i++;
      }
    }

    JsonArray binds = doc["bindings"].as<JsonArray>();
    if (!binds.isNull()) {
      for (JsonObject o : binds) {
        uint8_t buttonId = o["buttonId"] | 0;
        uint8_t action = o["action"] | 0;
        uint8_t param = o["param"] | 0;
        uint8_t effectIn = !o["effectIn"].isNull() ? o["effectIn"].as<uint8_t>()
                           : (!o["effect"].isNull() ? o["effect"].as<uint8_t>() : ShowMode::LEFT);
        uint8_t effectOut = !o["effectOut"].isNull() ? o["effectOut"].as<uint8_t>()
                            : (!o["effect2"].isNull() ? o["effect2"].as<uint8_t>() : ShowMode::LEFT);
        uint8_t speed = o["speed"].isNull() ? 0 : o["speed"].as<uint8_t>();
        RemoteMap::setBinding(buttonId, action, param, effectIn, effectOut, speed);
      }
      RemoteMap::save();
    }

    if (!doc["linkMac"].isNull()) {
      const char* mac = doc["linkMac"].as<const char*>();
      if (mac && strlen(mac) == 12) {
        Counters::setLinkedRemoteMac(mac);
      } else {
        Counters::clearLinkedRemoteMac();
      }
    }
  }

  void handlePostState() {
    String body = server.arg("plain");
    if (body.length() == 0) {
      // Some clients leave plain empty; try reconstructing from args
      body = server.arg(0);
    }
    if (body.length() == 0) {
      server.send(400, "text/plain", "no body");
      return;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
      server.send(400, "text/plain", String("bad json: ") + err.c_str());
      return;
    }

    applyStateDoc(doc);
    Counters::save();
    IdleCycle::noteActivity();
    Display::refreshAll();
    sendStateJson();
  }

  void handleLink() {
    String body = server.arg("plain");
    if (body.length() == 0) body = server.arg(0);
    if (body.length() == 0) {
      server.send(400, "text/plain", "no body");
      return;
    }
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
      server.send(400, "text/plain", String("bad json: ") + err.c_str());
      return;
    }
    const char* action = doc["action"] | "";
    if (strcmp(action, "link") == 0) {
      if (WizRemote::hasLastSeenMac()) {
        Counters::setLinkedRemoteMac(WizRemote::lastSeenMac());
        Counters::save();
      }
    } else if (strcmp(action, "clear") == 0) {
      Counters::clearLinkedRemoteMac();
      Counters::save();
    }
    sendStateJson();
  }

  void handleUpdateDone() {
    server.sendHeader("Connection", "close");
    if (Update.hasError()) {
      server.send(500, "text/plain", "Update failed");
    } else {
      server.send(200, "text/plain", "OK — rebooting");
      delay(400);
      ESP.restart();
    }
  }

  void handleUpdateUpload() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      Serial.printf("[ota-http] begin %s\n", upload.filename.c_str());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) {
        Serial.printf("[ota-http] success (%u bytes)\n", upload.totalSize);
      } else {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
      Update.end();
      Serial.println(F("[ota-http] aborted"));
    }
  }

  void handleWifiStatus() {
    JsonDocument doc;
    doc["apSsid"] = AP_SSID;
    doc["apIp"] = WifiMgr::apIp();
    doc["configured"] = WifiMgr::staConfigured();
    doc["connected"] = WifiMgr::staConnected();
    doc["status"] = WifiMgr::staStatusText();
    doc["ssid"] = WifiMgr::staSsid();
    doc["staIp"] = WifiMgr::staIp();
    doc["rssi"] = WifiMgr::staConnected() ? WifiMgr::staRssi() : 0;
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
  }

  void handleWifiScan() {
    const int16_t n = WifiMgr::scanNetworks();
    JsonDocument doc;
    JsonArray arr = doc["networks"].to<JsonArray>();
    const int16_t count = (n > 0) ? n : 0;
    for (int16_t i = 0; i < count; i++) {
      JsonObject o = arr.add<JsonObject>();
      o["ssid"] = WifiMgr::scanSsid(static_cast<uint8_t>(i));
      o["rssi"] = WifiMgr::scanRssi(static_cast<uint8_t>(i));
      o["secure"] = WifiMgr::scanSecure(static_cast<uint8_t>(i));
    }
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
  }

  void handleWifiConnect() {
    String body = server.arg("plain");
    if (body.length() == 0) body = server.arg(0);
    if (body.length() == 0) {
      server.send(400, "text/plain", "no body");
      return;
    }
    JsonDocument doc;
    if (deserializeJson(doc, body)) {
      server.send(400, "text/plain", "bad json");
      return;
    }
    const char* ssid = doc["ssid"] | "";
    const char* pass = doc["password"] | "";
    if (!ssid[0]) {
      server.send(400, "text/plain", "ssid required");
      return;
    }
    if (!WifiMgr::connectSta(ssid, pass)) {
      server.send(400, "text/plain", "invalid ssid");
      return;
    }
    server.send(200, "text/plain", "ok");
  }

  void handleWifiClear() {
    WifiMgr::clearSta();
    server.send(200, "text/plain", "ok");
  }

  void handleScroller() {
    String body = server.arg("plain");
    if (body.length() == 0) body = server.arg(0);
    JsonDocument doc;
    if (body.length() > 0 && deserializeJson(doc, body)) {
      server.send(400, "text/plain", "bad json");
      return;
    }
    const char* action = doc["action"] | "start";
    uint8_t effect = doc["effect"] | ShowMode::LEFT;
    if (strcmp(action, "stop") == 0) {
      Display::stopManualScroller();
      IdleCycle::noteActivity();
      server.send(200, "text/plain", "ok");
      return;
    }
    IdleCycle::noteActivity();
    Display::startManualScroller(effect, nullptr, nullptr, effect);
    server.send(200, "text/plain", "ok");
  }
}

namespace WebUI {

void begin() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/state", HTTP_GET, handleGetState);
  server.on("/api/state", HTTP_POST, handlePostState);
  server.on("/api/link", HTTP_POST, handleLink);
  server.on("/api/wifi/status", HTTP_GET, handleWifiStatus);
  server.on("/api/wifi/scan", HTTP_GET, handleWifiScan);
  server.on("/api/wifi/connect", HTTP_POST, handleWifiConnect);
  server.on("/api/wifi/clear", HTTP_POST, handleWifiClear);
  server.on("/api/scroller", HTTP_POST, handleScroller);
  server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
  server.begin();
  if (MDNS.begin(OTA_HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.print(F("[web] mDNS http://"));
    Serial.print(OTA_HOSTNAME);
    Serial.println(F(".local/"));
  }
  Serial.print(F("[web] UI SoftAP http://"));
  Serial.print(WifiMgr::apIp());
  Serial.println(F("/  (also on STA IP when joined)"));
}

void loop() {
  server.handleClient();
}

}  // namespace WebUI
