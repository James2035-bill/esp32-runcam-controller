#pragma once
#include <Arduino.h>

// Single-page control UI served at "/". Talks to the JSON API in
// esp32_project.ino. Built for Safari on iPhone: no pinch/double-tap zoom,
// 16px inputs (smaller makes iOS zoom on focus), large touch targets.

static const char WEB_UI[] PROGMEM = R"rawliteral(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1, user-scalable=no, viewport-fit=cover">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
<meta name="apple-mobile-web-app-title" content="RunCam">
<meta name="theme-color" content="#0d0f12">
<title>RunCam Remote</title>
<style>
  :root {
    --bg:#0d0f12; --panel:#171a1f; --panel-2:#20242b; --line:#2b3038;
    --text:#eef1f5; --muted:#8b93a0;
    --red:#ff3b30; --green:#30d158; --amber:#ffb340; --blue:#0a84ff;
    color-scheme: dark;
  }
  * { box-sizing:border-box; -webkit-tap-highlight-color:transparent; }
  html, body { margin:0; background:var(--bg); color:var(--text); }
  html { -webkit-text-size-adjust:100%; }
  body {
    font:16px/1.4 -apple-system, BlinkMacSystemFont, "SF Pro Text", system-ui, sans-serif;
    padding:max(16px, env(safe-area-inset-top)) 16px max(24px, env(safe-area-inset-bottom));
    max-width:520px; margin:0 auto;
    -webkit-user-select:none; user-select:none;
    touch-action:manipulation;
  }
  header { display:flex; align-items:center; justify-content:space-between; margin-bottom:16px; }
  h1 { font-size:22px; margin:0; letter-spacing:-0.02em; }
  .pill { display:inline-flex; align-items:center; gap:8px; padding:6px 12px;
          border-radius:999px; background:var(--panel); font-size:13px; color:var(--muted); }
  .dot { width:8px; height:8px; border-radius:50%; background:var(--red); }
  .pill.ok .dot { background:var(--green); }

  .card { background:var(--panel); border-radius:18px; padding:18px; margin-bottom:14px; }

  .hero { text-align:center; padding:26px 18px; }
  .state { font-size:14px; font-weight:600; text-transform:uppercase; letter-spacing:0.12em; color:var(--muted); }
  .hero.standby .state { color:var(--green); }
  .hero.busy .state { color:var(--amber); }
  .hero.rec .state { color:var(--red); }
  .hero.rec .state::before {
    content:""; display:inline-block; width:10px; height:10px; border-radius:50%;
    background:var(--red); margin-right:8px; animation:blink 1s infinite;
  }
  .timer { font:600 52px/1.1 ui-monospace, "SF Mono", Menlo, monospace; margin:8px 0 4px;
           font-variant-numeric:tabular-nums; }
  .hero.rec .timer { color:var(--red); }
  .sub { color:var(--muted); font-size:14px; min-height:20px; }

  .controls { display:grid; grid-template-columns:1fr 1fr; gap:12px; margin-bottom:14px; }
  button {
    font:inherit; font-weight:600; color:var(--text);
    border:0; border-radius:16px; background:var(--panel-2);
    padding:18px 12px; min-height:64px; cursor:pointer; touch-action:manipulation;
    transition:transform .06s, opacity .2s, background .2s;
  }
  button:active:not(:disabled) { transform:scale(0.97); }
  button:disabled { opacity:0.32; cursor:default; }
  .big { grid-column:1 / -1; min-height:96px; font-size:22px; border-radius:22px; }
  .rec-btn { background:var(--red); }
  .stop-btn { background:#fff; color:#000; }
  .standby-btn { background:var(--green); color:#00210a; }
  .off-btn.confirm { background:var(--amber); color:#2a1a00; }

  .toast { min-height:20px; font-size:14px; color:var(--muted); text-align:center; margin:-4px 0 12px; }
  .toast.err { color:var(--amber); }

  .stats { display:grid; grid-template-columns:repeat(3, 1fr); gap:10px; }
  .stat .k { font-size:12px; color:var(--muted); }
  .stat .v { font-size:20px; font-weight:600; font-variant-numeric:tabular-nums; }
  .bar { height:8px; border-radius:4px; background:var(--panel-2); margin:14px 0 4px; overflow:hidden; }
  .bar > i { display:block; height:100%; width:0; background:var(--green); transition:width .4s; }
  .bar.low > i { background:var(--amber); }
  .bar.crit > i { background:var(--red); }
  .note { font-size:13px; color:var(--muted); margin:8px 0 0; }

  details { background:var(--panel); border-radius:18px; margin-bottom:14px; }
  summary { padding:16px 18px; font-weight:600; cursor:pointer; list-style:none; }
  summary::-webkit-details-marker { display:none; }
  summary::after { content:"\203A"; float:right; color:var(--muted); transition:transform .2s; }
  details[open] summary::after { transform:rotate(90deg); }
  .inner { padding:0 18px 18px; }
  h3 { font-size:13px; text-transform:uppercase; letter-spacing:.08em; color:var(--muted); margin:6px 0 10px; }
  .row { display:flex; gap:8px; margin-bottom:10px; align-items:center; }
  .row label { flex:1; font-size:14px; color:var(--muted); }
  input {
    font-size:16px; color:var(--text); background:var(--panel-2); border:1px solid var(--line);
    border-radius:10px; padding:10px 12px; width:130px;
    -webkit-user-select:text; user-select:text;
  }
  .row button, .small { min-height:0; padding:10px 14px; border-radius:10px; font-size:14px; }
  .log {
    font:12px/1.45 ui-monospace, "SF Mono", Menlo, monospace; color:#b9c0cb;
    background:#0a0b0e; border-radius:10px; padding:10px; height:220px; overflow-y:auto;
    white-space:pre-wrap; word-break:break-word; -webkit-user-select:text; user-select:text;
  }
  .log .me { color:var(--blue); }

  @keyframes blink { 50% { opacity:0.25; } }
</style>
</head>
<body>

<header>
  <h1>RunCam Remote</h1>
  <span id="conn" class="pill"><span class="dot"></span><span id="connText">Connecting…</span></span>
</header>

<section id="hero" class="card hero">
  <div id="stateText" class="state">–</div>
  <div id="timer" class="timer">00:00</div>
  <div id="subText" class="sub">&nbsp;</div>
</section>

<section class="controls">
  <button id="recBtn"  class="big rec-btn" disabled>&#9679; Start recording</button>
  <button id="stopBtn" class="big stop-btn" hidden disabled>&#9632; Stop &amp; save</button>
  <button id="standbyBtn" class="standby-btn" disabled>Standby</button>
  <button id="offBtn" class="off-btn" disabled>Power off</button>
</section>
<div id="toast" class="toast"></div>

<section class="card">
  <div class="stats">
    <div class="stat"><div class="k">Battery</div><div class="v" id="soc">–</div></div>
    <div class="stat"><div class="k">Pack</div><div class="v" id="volts">–</div></div>
    <div class="stat"><div class="k" id="rtLabel">Remaining</div><div class="v" id="rt">–</div></div>
  </div>
  <div id="bar" class="bar"><i></i></div>
  <p class="note" id="batNote">&nbsp;</p>
</section>

<details>
  <summary>Settings</summary>
  <div class="inner">
    <h3>Battery</h3>
    <div class="row"><label>Capacity (mAh)</label><input id="capIn" inputmode="numeric"><button data-set="CAP" data-in="capIn">Set</button></div>
    <div class="row"><label>Camera power (mW)</label><input id="wIn" inputmode="numeric"><button data-set="WATTS" data-in="wIn">Set</button></div>
    <div class="row"><label>Measured draw (mA)</label><input id="drawIn" inputmode="numeric" placeholder="e.g. 140"><button data-set="DRAW" data-in="drawIn">Set</button></div>
    <div class="row"><label>Cells (0 = auto)</label><input id="cellsIn" inputmode="numeric"><button data-set="CELLS" data-in="cellsIn">Set</button></div>
    <div class="row"><label>Reserve (%)</label><input id="resIn" inputmode="numeric"><button data-set="RESERVE" data-in="resIn">Set</button></div>
    <h3 style="margin-top:18px">Wi-Fi</h3>
    <div class="row"><label>Network name</label><input id="ssidIn" autocapitalize="off" autocorrect="off"><button data-set="SSID" data-in="ssidIn" data-text="1">Set</button></div>
    <div class="row"><label>Password (8+)</label><input id="passIn" type="password" autocapitalize="off" autocorrect="off"><button data-set="PASS" data-in="passIn" data-text="1">Set</button></div>
    <p class="note">Saved on the ESP32. Wi-Fi changes apply after a restart (camera must be off), then rejoin the new network.</p>
    <div class="row" style="margin-top:10px"><button class="small" data-cmd="RESTART">Restart ESP32</button></div>
  </div>
</details>

<details>
  <summary>Console</summary>
  <div class="inner">
    <div class="row">
      <button class="small" data-cmd="STATUS">Status</button>
      <button class="small" data-cmd="INFO">Camera info</button>
      <button class="small" data-cmd="RUNTIME">Runtime</button>
      <button class="small" data-cmd="MODE">Mode</button>
    </div>
    <div id="log" class="log"></div>
    <div class="row" style="margin-top:10px">
      <input id="cmdIn" style="flex:1;width:auto" placeholder="Command, e.g. HELP" autocapitalize="characters" autocomplete="off" autocorrect="off">
      <button id="sendBtn">Send</button>
    </div>
  </div>
</details>

<script>
const $ = (id) => document.getElementById(id);
let st = null;              // last status from the ESP32
let online = false, fails = 0;
let recStart = 0;           // local clock time recording began
let busy = false;           // a control request is in flight
let filled = false;         // settings inputs pre-filled once

function log(text, mine) {
  const el = $("log");
  for (const t of String(text).split("\n")) {
    const d = document.createElement("div");
    if (mine) d.className = "me";
    d.textContent = (mine ? "> " : "") + t;
    el.appendChild(d);
  }
  while (el.childNodes.length > 300) el.removeChild(el.firstChild);
  el.scrollTop = el.scrollHeight;
}
function toast(msg, err) {
  const t = $("toast");
  t.textContent = msg || "";
  t.className = "toast" + (err ? " err" : "");
}

// --------------------------------------------------------------- API ---
async function poll() {
  try {
    const r = await fetch("/api/status", { cache: "no-store" });
    apply(await r.json());
    fails = 0;
    setOnline(true);
  } catch (e) {
    if (++fails >= 2) setOnline(false);
  }
}

async function post(path, body) {
  busy = true; render();
  try {
    const r = await fetch(path, {
      method: "POST",
      headers: { "Content-Type": "application/x-www-form-urlencoded" },
      body: body ? new URLSearchParams(body) : "",
    });
    const j = await r.json();
    if (j.msg) log(j.msg);
    toast(j.msg ? j.msg.split("\n").pop() : "", !j.ok);
    if (j.status) apply(j.status);
    return j;
  } catch (e) {
    toast("No response — is the iPhone still on the RunCam Wi-Fi?", true);
    setOnline(false);
  } finally {
    busy = false; render();
  }
}

function cmd(c) { log(c, true); return post("/api/cmd", { c }); }

function setOnline(ok) {
  online = ok;
  $("conn").className = "pill" + (ok ? " ok" : "");
  $("connText").textContent = ok ? "Connected" : "Not connected";
  render();
}

function apply(s) {
  const wasRec = st && st.recording;
  st = s;
  if (s.recording) {
    const fromDevice = Date.now() - s.recSec * 1000;
    if (!wasRec || Math.abs(fromDevice - recStart) > 2000) recStart = fromDevice;
  }
  if (!filled && s.settings) {
    filled = true;
    $("capIn").value = s.settings.capacityMah;
    $("wIn").value = s.settings.camPowerMw;
    $("cellsIn").value = s.settings.cells;
    $("resIn").value = s.settings.reservePct;
    $("ssidIn").value = s.settings.ssid;
  }
  render();
}

// ---------------------------------------------------------------- UI ---
function clock(sec) {
  sec = Math.max(0, Math.floor(sec));
  const h = Math.floor(sec / 3600), m = Math.floor(sec / 60) % 60, s = sec % 60;
  const mm = String(m).padStart(2, "0"), ss = String(s).padStart(2, "0");
  return h ? `${h}:${mm}:${ss}` : `${mm}:${ss}`;
}
function mins(n) {
  n = Math.max(0, Math.round(n || 0));
  return n >= 60 ? `${Math.floor(n / 60)}h ${String(n % 60).padStart(2, "0")}m` : `${n}m`;
}

const LABEL = {
  OFF: "Camera off", BOOTING: "Starting…", STANDBY: "Standby",
  RECORDING: "Recording", SAVING: "Saving file…", STOPPING: "Saving, then off…",
};

function render() {
  const s = online ? st : null;
  const state = s ? s.state : null;
  const rec = state === "RECORDING";

  const hero = $("hero");
  hero.className = "card hero" +
    (rec ? " rec" : state === "STANDBY" ? " standby" :
     ["BOOTING", "SAVING", "STOPPING"].includes(state) ? " busy" : "");
  $("stateText").textContent = s ? LABEL[state] || state : "Not connected";
  $("timer").textContent = rec ? clock((Date.now() - recStart) / 1000) : "00:00";
  $("subText").textContent =
    !s ? "Join the RunCam Wi-Fi, then reload" :
    state === "OFF" ? "Tap Standby to power the camera" :
    state === "BOOTING" ? "Camera is starting up" :
    state === "STANDBY" && !s.uart ? "Camera not answering over UART — check wiring" :
    state === "STANDBY" ? "Ready to record" :
    state === "SAVING" ? "Finalising the video file — don't cut power" :
    state === "STOPPING" ? "Power cuts once the file is saved" :
    `Camera on ${mins(s.onMin)} this session`;

  $("recBtn").hidden = rec;
  $("stopBtn").hidden = !rec;
  $("recBtn").disabled = busy || state !== "STANDBY";
  $("stopBtn").disabled = busy || !rec;
  $("standbyBtn").disabled = busy || state !== "OFF";
  $("offBtn").disabled = busy || !state || ["OFF", "STOPPING"].includes(state);

  if (s && !s.vbat) {
    // No divider fitted (or an implausible reading): show no numbers at all
    // rather than whatever a floating ADC pin happens to read.
    $("soc").textContent = $("volts").textContent = "–";
    $("rtLabel").textContent = "Full pack";
    $("rt").textContent = mins(s.fullMin);
    $("bar").className = "bar";
    $("bar").firstElementChild.style.width = "0";
    $("batNote").textContent = `No battery sensor · full-pack time is from ${s.settings.capacityMah} mAh, not measured`;
  } else if (s) {
    $("soc").textContent = s.soc + "%";
    $("volts").textContent = s.volts.toFixed(1) + " V";
    const off = state === "OFF";
    $("rtLabel").textContent = off ? "Full pack" : "Remaining";
    $("rt").textContent = mins(off ? s.fullMin : s.remainingMin);
    $("bar").className = "bar" + (s.soc <= 15 ? " crit" : s.soc <= 30 ? " low" : "");
    $("bar").firstElementChild.style.width = Math.min(100, Math.max(0, s.soc)) + "%";
    $("batNote").textContent = `${s.cells}S pack · estimate from voltage; reads low under load`;
  } else {
    $("soc").textContent = $("volts").textContent = $("rt").textContent = "–";
    $("bar").firstElementChild.style.width = "0";
    $("batNote").innerHTML = "&nbsp;";
  }
}

// Power off needs a second tap -- it cuts the camera's supply.
let offArmed = null;
function powerOff() {
  const b = $("offBtn");
  if (!offArmed) {
    b.classList.add("confirm");
    b.textContent = "Tap to confirm";
    offArmed = setTimeout(disarm, 3000);
    return;
  }
  disarm();
  post("/api/off");
}
function disarm() {
  clearTimeout(offArmed); offArmed = null;
  $("offBtn").classList.remove("confirm");
  $("offBtn").textContent = "Power off";
}

$("standbyBtn").onclick = () => post("/api/standby");
$("recBtn").onclick     = () => post("/api/record");
$("stopBtn").onclick    = () => post("/api/stop");
$("offBtn").onclick     = powerOff;

document.querySelectorAll("[data-cmd]").forEach((b) => b.onclick = () => cmd(b.dataset.cmd));
document.querySelectorAll("[data-set]").forEach((b) => b.onclick = () => {
  const input = $(b.dataset.in);
  const val = input.value.trim();
  if (!b.dataset.text && !/^\d+$/.test(val)) { toast("Enter a whole number", true); return; }
  if (!val) return;
  cmd(`${b.dataset.set} ${val}`);
  if (b.dataset.set === "DRAW" || b.dataset.set === "PASS") input.value = "";
});
function sendTyped() {
  const v = $("cmdIn").value.trim();
  if (v) { cmd(v); $("cmdIn").value = ""; }
}
$("sendBtn").onclick = sendTyped;
$("cmdIn").addEventListener("keydown", (e) => { if (e.key === "Enter") sendTyped(); });

// Poll faster while the camera is changing state.
async function loop() {
  await poll();
  const fast = st && ["BOOTING", "SAVING", "STOPPING"].includes(st.state);
  setTimeout(loop, fast ? 400 : 1000);
}
setInterval(() => { if (online && st && st.recording) render(); }, 250);
document.addEventListener("visibilitychange", () => { if (!document.hidden) poll(); });
loop();
</script>
</body>
</html>
)rawliteral";
