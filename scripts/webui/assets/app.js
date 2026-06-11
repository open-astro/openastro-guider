/* OpenAstro Guider web UI — drives the JSON-RPC API at /api/rpc.
   Vanilla JS, no dependencies, no build step. */
"use strict";

const $ = (id) => document.getElementById(id);

/* ---------------- rpc + toasts ---------------- */

async function rpc(method, params) {
  const body = params === undefined ? { method } : { method, params };
  const res = await fetch("/api/rpc", { method: "POST", body: JSON.stringify(body) });
  const j = await res.json();
  if (!j.ok) throw new Error(j.error || "rpc failed");
  return j.result;
}

// rpc that swallows errors (for pollers — a transient failure must not spam)
async function rpcQuiet(method, params) {
  try { return await rpc(method, params); } catch (e) { return undefined; }
}

function toast(msg, ok) {
  const el = document.createElement("div");
  el.className = "toast" + (ok ? " ok" : "");
  el.textContent = msg;
  $("toasts").appendChild(el);
  setTimeout(() => el.remove(), ok ? 2500 : 6000);
}

// wrap a button action: disable while running, toast errors
function action(btnId, fn) {
  const btn = $(btnId);
  btn.addEventListener("click", async () => {
    btn.disabled = true;
    try { await fn(); } catch (e) { toast(e.message); }
    btn.disabled = false;
  });
}

/* ---------------- tabs ---------------- */

let activeTab = "guide";
document.querySelectorAll("nav#tabs button").forEach((b) => {
  b.addEventListener("click", () => {
    document.querySelectorAll("nav#tabs button").forEach((x) => x.classList.remove("active"));
    document.querySelectorAll(".tab").forEach((x) => x.classList.remove("active"));
    b.classList.add("active");
    $("tab-" + b.dataset.tab).classList.add("active");
    activeTab = b.dataset.tab;
    if (activeTab === "equipment") loadEquipment();
    if (activeTab === "settings") loadSettings();
    if (activeTab === "darks") loadDarks();
  });
});

document.querySelectorAll(".polar-picker button").forEach((b) => {
  b.addEventListener("click", () => {
    document.querySelectorAll(".polar-picker button").forEach((x) => x.classList.remove("active"));
    document.querySelectorAll(".pa-panel").forEach((x) => x.classList.remove("active"));
    b.classList.add("active");
    $("pa-" + b.dataset.pa).classList.add("active");
    activePa = b.dataset.pa;
  });
});
let activePa = "staticpa";

/* ---------------- header status poller ---------------- */

let appState = "—";
let connected = false;

async function pollHeader() {
  const st = await rpcQuiet("get_app_state");
  if (st !== undefined) {
    appState = st;
    const pill = $("pill-state");
    pill.textContent = st;
    pill.className = "pill state-" + st;
  }
  const conn = await rpcQuiet("get_connected");
  if (conn !== undefined) {
    connected = conn;
    const c = $("pill-conn");
    c.textContent = conn ? "● connected" : "○ disconnected";
    c.className = "pill " + (conn ? "on" : "off");
  }
}

/* ---------------- guide tab ---------------- */

let frameSize = null; // [w, h]
let lockPos = null;
let starsOverlay = [];

// Live frame: polled on its own fast timer (not the 1.2 s main tick) so the
// view tracks looping closely. The server answers 304 via the frame-counter
// ETag when nothing new arrived, so most polls cost almost nothing.
let frameEtag = null;
let frameBusy = false;

async function refreshFrame() {
  if (shutDown || activeTab !== "guide" || frameBusy) return;
  frameBusy = true;
  try {
    const res = await fetch("/api/frame.jpg", { headers: frameEtag ? { "If-None-Match": frameEtag } : {} });
    if (res.status === 200) {
      frameEtag = res.headers.get("ETag");
      const url = URL.createObjectURL(await res.blob());
      const img = $("frame");
      const probe = new Image();
      probe.onload = () => {
        const prev = img.src;
        img.src = url;
        $("frame-msg").style.display = "none";
        drawOverlay(probe.naturalWidth, probe.naturalHeight);
        if (prev.startsWith("blob:")) URL.revokeObjectURL(prev);
      };
      probe.src = url;
    }
  } catch (e) {
    // daemon unreachable; keep the previous image
  } finally {
    frameBusy = false;
  }
}

setInterval(refreshFrame, 400);

async function pollGuide() {
  lockPos = await rpcQuiet("get_lock_position");

  if ($("show-stars").checked) {
    const stars = await rpcQuiet("get_star_centroids", { max_stars: 20 });
    starsOverlay = stars || [];
  } else {
    starsOverlay = [];
  }

  // graph + stats
  const hist = await rpcQuiet("get_guide_history", { max_points: 200 });
  if (hist) drawGraph(hist);

  // pause button state
  const paused = await rpcQuiet("get_paused");
  if (paused !== undefined) $("btn-pause").classList.toggle("on", paused);
}

function drawOverlay(natW, natH) {
  const canvas = $("frame-overlay");
  const wrap = $("frame-wrap");
  canvas.width = wrap.clientWidth;
  canvas.height = wrap.clientHeight;
  const ctx = canvas.getContext("2d");
  ctx.clearRect(0, 0, canvas.width, canvas.height);
  if (!natW || !natH) return;
  const sx = canvas.width / natW;
  const sy = canvas.height / natH;

  if (Array.isArray(starsOverlay)) {
    ctx.strokeStyle = "rgba(110,170,255,0.8)";
    ctx.lineWidth = 1;
    for (const s of starsOverlay) {
      ctx.beginPath();
      ctx.arc(s.x * sx, s.y * sy, 8, 0, Math.PI * 2);
      ctx.stroke();
    }
  }
  if (Array.isArray(lockPos) && lockPos.length === 2) {
    const x = lockPos[0] * sx, y = lockPos[1] * sy;
    ctx.strokeStyle = "#43c878";
    ctx.lineWidth = 1.5;
    ctx.beginPath();
    ctx.moveTo(x - 14, y); ctx.lineTo(x - 4, y);
    ctx.moveTo(x + 4, y); ctx.lineTo(x + 14, y);
    ctx.moveTo(x, y - 14); ctx.lineTo(x, y - 4);
    ctx.moveTo(x, y + 4); ctx.lineTo(x, y + 14);
    ctx.stroke();
    ctx.strokeRect(x - 9, y - 9, 18, 18);
  }
}

function drawGraph(hist) {
  const canvas = $("graph");
  canvas.width = canvas.clientWidth * devicePixelRatio;
  canvas.height = 260 * devicePixelRatio;
  const ctx = canvas.getContext("2d");
  ctx.scale(devicePixelRatio, devicePixelRatio);
  const W = canvas.clientWidth, H = 260;
  ctx.clearRect(0, 0, W, H);

  const pts = hist.points || [];
  const scale = hist.pixel_scale && hist.pixel_scale !== 1 ? hist.pixel_scale : 0;
  const useArcsec = $("graph-arcsec").checked && scale;
  const unit = useArcsec ? '"' : "px";
  const k = useArcsec ? scale : 1;

  // y range: nice number >= data peak, min 1
  let peak = 1;
  for (const p of pts) peak = Math.max(peak, Math.abs(p.ra * k), Math.abs(p.dec * k));
  const yMax = Math.ceil(peak * 1.15 * 2) / 2;
  const y0 = H / 2;
  const yScale = (H / 2 - 18) / yMax;

  // grid
  ctx.strokeStyle = "#1b2333";
  ctx.fillStyle = "#5b6678";
  ctx.font = "10px ui-monospace, monospace";
  ctx.lineWidth = 1;
  for (let g = -2; g <= 2; g++) {
    const y = y0 - (g * yMax / 2) * yScale;
    ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(W, y); ctx.stroke();
    if (g) ctx.fillText((g * yMax / 2).toFixed(1) + unit, 4, y - 3);
  }
  ctx.strokeStyle = "#2a3447";
  ctx.beginPath(); ctx.moveTo(0, y0); ctx.lineTo(W, y0); ctx.stroke();

  if (pts.length >= 2) {
    const dx = W / Math.max(pts.length - 1, 1);

    // correction bars
    if ($("graph-corr").checked) {
      const maxDur = Math.max(100, ...pts.map((p) => Math.max(p.ra_dur, p.dec_dur)));
      const dScale = (H / 2 - 20) / maxDur;
      for (let i = 0; i < pts.length; i++) {
        const p = pts[i], x = i * dx;
        if (p.ra_dur) {
          const sign = p.ra_dir === "E" ? 1 : -1;
          ctx.fillStyle = "rgba(74,157,232,0.35)";
          const h = p.ra_dur * dScale * sign;
          ctx.fillRect(x - 1.5, h > 0 ? y0 - h : y0, 3, Math.abs(h));
        }
        if (p.dec_dur) {
          const sign = p.dec_dir === "N" ? 1 : -1;
          ctx.fillStyle = "rgba(232,93,74,0.35)";
          const h = p.dec_dur * dScale * sign;
          ctx.fillRect(x + 2, h > 0 ? y0 - h : y0, 3, Math.abs(h));
        }
      }
    }

    const line = (key, color) => {
      ctx.strokeStyle = color;
      ctx.lineWidth = 1.6;
      ctx.beginPath();
      for (let i = 0; i < pts.length; i++) {
        const y = y0 - pts[i][key] * k * yScale;
        i ? ctx.lineTo(i * dx, y) : ctx.moveTo(0, y);
      }
      ctx.stroke();
    };
    line("ra", "#4a9de8");
    line("dec", "#e85d4a");
  }

  // legend
  ctx.fillStyle = "#4a9de8"; ctx.fillRect(W - 92, 8, 10, 3);
  ctx.fillStyle = "#8b96a8"; ctx.fillText("RA", W - 78, 13);
  ctx.fillStyle = "#e85d4a"; ctx.fillRect(W - 56, 8, 10, 3);
  ctx.fillStyle = "#8b96a8"; ctx.fillText("Dec", W - 42, 13);

  // stats line
  const s = hist.stats || {};
  const f = (v) => (v * k).toFixed(2);
  const rms = $("pill-rms");
  if (s.n > 1) {
    rms.textContent = `RMS ${f(s.rms_tot)}${unit}`;
    $("stats").innerHTML =
      `<span>RMS RA <b>${f(s.rms_ra)}${unit}</b></span>` +
      `<span>RMS Dec <b>${f(s.rms_dec)}${unit}</b></span>` +
      `<span>RMS total <b>${f(s.rms_tot)}${unit}</b></span>` +
      `<span>peak RA <b>${f(s.ra_peak)}${unit}</b></span>` +
      `<span>peak Dec <b>${f(s.dec_peak)}${unit}</b></span>` +
      `<span>osc <b>${(s.osc_index ?? 0).toFixed(2)}</b></span>` +
      (pts.length ? `<span>SNR <b>${pts[pts.length - 1].snr.toFixed(1)}</b></span>` : "") +
      (s.star_lost ? `<span class="bad">star lost <b>${s.star_lost}</b></span>` : "");
  } else {
    rms.textContent = "RMS —";
    $("stats").textContent = "";
  }
}

async function loadExposures() {
  const durs = await rpcQuiet("get_exposure_durations");
  const cur = await rpcQuiet("get_exposure");
  const sel = $("exposure");
  sel.innerHTML = "";
  (durs || []).filter((d) => d > 0).forEach((d) => {
    const o = document.createElement("option");
    o.value = d;
    o.textContent = d >= 1000 ? (d / 1000) + " s" : d + " ms";
    if (d === cur) o.selected = true;
    sel.appendChild(o);
  });
}
$("exposure").addEventListener("change", () =>
  rpc("set_exposure", { exposure: parseInt($("exposure").value, 10) }).catch((e) => toast(e.message)));

action("btn-loop", () => rpc("loop"));
action("btn-stop", () => rpc("stop_capture"));
action("btn-select", async () => { const p = await rpc("find_star"); toast(`star at ${p[0].toFixed(1)}, ${p[1].toFixed(1)}`, true); });
action("btn-guide", () => rpc("guide", { settle: { pixels: 1.5, time: 8, timeout: 60 } }));
action("btn-pause", async () => {
  const paused = await rpc("get_paused");
  await rpc("set_paused", paused ? { paused: false } : { paused: true });
});
action("btn-dither", () => rpc("dither", { amount: 3, raOnly: false, settle: { pixels: 1.5, time: 8, timeout: 60 } }));

/* ---------------- polar align: static PA ---------------- */

async function pollStaticPa() {
  const st = await rpcQuiet("staticpa_get_status");
  if (!st) return;
  if (st.active && st.ref_stars && $("spa-refstar").options.length !== st.ref_stars.length) {
    const sel = $("spa-refstar");
    sel.innerHTML = "";
    st.ref_stars.forEach((s) => {
      const o = document.createElement("option");
      o.value = s.index;
      o.textContent = `${s.name} (mag ${s.mag})`;
      if (s.index === st.ref_star) o.selected = true;
      sel.appendChild(o);
    });
  }

  const out = [];
  out.push(["tool", st.active ? "open" : "closed"]);
  if (st.active) {
    out.push(["collecting", st.aligning ? "yes" : "no"]);
    out.push(["points", (st.measured_points || []).map((p) => `#${p.position}`).join(" ") || "none"]);
    if (st.rotation)
      out.push(["rotation", `${st.rotation.rotated_deg.toFixed(1)} / ${st.rotation.required_deg.toFixed(1)}° step ${st.rotation.step}/${st.rotation.required_steps}${st.rotation.slewing ? " (slewing)" : ""}`]);
    if (st.calced && st.centre)
      out.push(["centre of rotation", `${st.centre.x.toFixed(1)}, ${st.centre.y.toFixed(1)}  r=${st.centre.radius_px.toFixed(1)}px`]);
    const adj = st.live_adjustment || st.adjustment;
    if (adj) {
      out.push(["alt error", `<b class="${Math.abs(adj.alt_error_arcmin) < 1 ? "good" : "bad"}">${adj.alt_error_arcmin.toFixed(2)}′</b>`]);
      out.push(["az error", `<b class="${Math.abs(adj.az_error_arcmin) < 1 ? "good" : "bad"}">${adj.az_error_arcmin.toFixed(2)}′</b>`]);
      out.push(["total", `<b>${adj.total_error_arcmin.toFixed(2)}′</b>`]);
    }
  }
  $("spa-readout").innerHTML = out.map(([k, v]) => `<span>${k}</span><b>${v}</b>`).join("");
  drawTargetCanvas($("spa-canvas"), st.current_star, st.ref_star_target, st.live_adjustment);
}

// shared "drive the dot to the bullseye" canvas
function drawTargetCanvas(canvas, current, target, adj) {
  const ctx = canvas.getContext("2d");
  const W = canvas.width, H = canvas.height;
  ctx.clearRect(0, 0, W, H);
  if (!current || !target) {
    ctx.fillStyle = "#5b6678";
    ctx.font = "12px system-ui";
    ctx.fillText("waiting for measurement…", 14, 24);
    return;
  }
  // center the target, place the star relative to it (auto zoom)
  const dx = current.x - target.x, dy = current.y - target.y;
  const dist = Math.hypot(dx, dy);
  const zoom = Math.min((Math.min(W, H) / 2 - 30) / Math.max(dist, 1), 8);
  const cx = W / 2, cy = H / 2;

  // bullseye
  ctx.strokeStyle = "#43c878";
  for (const r of [8, 20, 36]) { ctx.beginPath(); ctx.arc(cx, cy, r, 0, Math.PI * 2); ctx.stroke(); }

  const sx = cx + dx * zoom, sy = cy + dy * zoom;
  // connecting line + star
  ctx.strokeStyle = "rgba(139,150,168,0.6)";
  ctx.setLineDash([4, 4]);
  ctx.beginPath(); ctx.moveTo(sx, sy); ctx.lineTo(cx, cy); ctx.stroke();
  ctx.setLineDash([]);
  ctx.fillStyle = "#e8b84a";
  ctx.beginPath(); ctx.arc(sx, sy, 5, 0, Math.PI * 2); ctx.fill();

  // alt/az correction vectors from the star, if available
  if (adj && adj.alt_vector && adj.az_vector) {
    const arrow = (vx, vy, color, label) => {
      const ex = sx + vx * zoom, ey = sy + vy * zoom;
      ctx.strokeStyle = color; ctx.fillStyle = color; ctx.lineWidth = 1.6;
      ctx.beginPath(); ctx.moveTo(sx, sy); ctx.lineTo(ex, ey); ctx.stroke();
      ctx.font = "11px system-ui";
      ctx.fillText(label, ex + 4, ey);
      ctx.lineWidth = 1;
    };
    arrow(adj.alt_vector.x, adj.alt_vector.y, "#e85d4a", "Alt");
    arrow(adj.az_vector.x, adj.az_vector.y, "#4a9de8", "Az");
  }

  ctx.fillStyle = "#5b6678";
  ctx.font = "11px ui-monospace, monospace";
  ctx.fillText(`zoom ×${zoom.toFixed(1)}  sep ${dist.toFixed(1)}px`, 10, H - 10);
}

action("spa-start", async () => {
  const params = {
    auto: $("spa-auto").value === "true",
    hemisphere: $("spa-hemi").value,
    hour_angle: parseFloat($("spa-ha").value) || 0,
    flip_camera: $("spa-flip").checked,
  };
  const idx = parseInt($("spa-refstar").value, 10);
  if (!isNaN(idx)) params.ref_star = idx;
  await rpc("staticpa_start", params);
  $("spa-msg").textContent = "";
  toast("static PA started", true);
});
action("spa-m2", () => rpc("staticpa_measure", { position: 2 }));
action("spa-m3", () => rpc("staticpa_measure", { position: 3 }));
action("spa-stop", () => rpc("staticpa_stop"));
action("spa-close", () => rpc("staticpa_close"));

/* ---------------- polar align: polar drift ---------------- */

async function pollPolarDrift() {
  const st = await rpcQuiet("polardrift_get_status");
  if (!st) return;
  const out = [["tool", st.active ? "open" : "closed"]];
  if (st.active) {
    out.push(["measuring", st.drifting ? "yes" : "no"]);
    out.push(["samples", st.num_samples ?? 0]);
    if (st.elapsed_s !== undefined) out.push(["elapsed", st.elapsed_s.toFixed(0) + " s"]);
    if (st.error_arcmin !== undefined) {
      out.push(["PA error", `<b class="${st.error_arcmin < 1 ? "good" : "bad"}">${st.error_arcmin.toFixed(2)}′</b>`]);
      out.push(["pole direction", st.pole_direction_deg.toFixed(1) + "°"]);
    }
  }
  $("pd-readout").innerHTML = out.map(([k, v]) => `<span>${k}</span><b>${v}</b>`).join("");
  drawTargetCanvas($("pd-canvas"), st.current_star, st.target, null);
}

action("pd-start", () => rpc("polardrift_start", { hemisphere: $("pd-hemi").value, mirrored: $("pd-mirror").checked }));
action("pd-stop", () => rpc("polardrift_stop"));
action("pd-close", () => rpc("polardrift_close"));

/* ---------------- polar align: drift align ---------------- */

async function pollDriftAlign() {
  const st = await rpcQuiet("driftalign_get_status");
  if (!st) return;
  const out = [["tool", st.active ? "open" : "closed"]];
  if (st.active) {
    out.push(["phase", st.phase]);
    out.push(["mode", st.mode]);
    out.push(["calibrated", st.calibrated ? "yes" : '<b class="bad">no — calibrate first</b>']);
    out.push(["guiding", st.guiding ? "yes" : "no"]);
    if (st.status_message) out.push(["status", st.status_message]);
    if (st.scope) out.push(["scope", `RA ${st.scope.ra_hours.toFixed(2)}h  Dec ${st.scope.dec_degrees.toFixed(1)}°`]);
    const err = st.polar_alignment_error;
    $("da-error").innerHTML = err
      ? `${Math.abs(err.error_arcmin).toFixed(2)}<small>′ ${st.phase} error · ${err.samples} samples</small>`
      : `—<small> drift to measure</small>`;
    if (err) out.push(["dec drift", err.dec_drift_arcsec_per_min.toFixed(2) + "″/min"]);
  } else {
    $("da-error").textContent = "—";
  }
  $("da-readout").innerHTML = out.map(([k, v]) => `<span>${k}</span><b>${v}</b>`).join("");
}

action("da-start", () => rpc("driftalign_start"));
action("da-drift", () => rpc("driftalign_drift"));
action("da-adjust", () => rpc("driftalign_adjust"));
action("da-close", () => rpc("driftalign_close"));
$("da-phase").addEventListener("change", () =>
  rpc("driftalign_set_phase", { phase: $("da-phase").value }).catch((e) => toast(e.message)));

/* ---------------- equipment tab ---------------- */

async function loadEquipment() {
  const profiles = await rpcQuiet("get_profiles");
  if (profiles) {
    const sel = $("eq-profile");
    sel.innerHTML = "";
    profiles.forEach((p) => {
      const o = document.createElement("option");
      o.value = p.id;
      o.textContent = p.name;
      if (p.selected) o.selected = true;
      sel.appendChild(o);
    });
  }
  const setup = await rpcQuiet("get_profile_setup");
  if (setup) {
    $("eq-focal").value = setup.focal_length ?? "";
    $("eq-pixsize").value = setup.pixel_size ?? "";
  }
  const scale = await rpcQuiet("get_pixel_scale");
  $("eq-scale").textContent = scale ? scale.toFixed(2) + " ″/px" : "unknown — set focal length + pixel size";
  const alpaca = await rpcQuiet("get_alpaca_server");
  if (alpaca) { $("eq-host").value = alpaca.host || ""; $("eq-port").value = alpaca.port || ""; }

  const choices = await rpcQuiet("get_equipment_choices");
  const fill = async (id, list, getter) => {
    const cur = await rpcQuiet(getter);
    const sel = $(id);
    sel.innerHTML = "";
    (list || []).forEach((c) => {
      const o = document.createElement("option");
      o.textContent = c;
      if (c === cur) o.selected = true;
      sel.appendChild(o);
    });
  };
  if (choices) {
    await fill("eq-camera", choices.camera, "get_selected_camera");
    await fill("eq-mount", choices.mount, "get_selected_mount");
    await fill("eq-rotator", choices.rotator, "get_selected_rotator");
  }
  const cal = await rpcQuiet("get_calibrated");
  $("eq-cal").textContent = cal === undefined ? "—" : cal ? "calibrated" : "not calibrated";
}

$("eq-profile").addEventListener("change", async () => {
  try {
    await rpc("set_profile", { id: parseInt($("eq-profile").value, 10) });
    toast("profile selected", true);
    loadEquipment();
  } catch (e) { toast(e.message); }
});

action("eq-prof-new", async () => {
  const name = prompt("Name for the new profile:");
  if (!name) return;
  const copy = $("eq-profile").options.length > 0 &&
    confirm("Copy settings from the current profile?\n(Cancel starts from defaults.)");
  const params = { name, select: true };
  if (copy) params.copy_from_id = parseInt($("eq-profile").value, 10);
  await rpc("create_profile", params);
  toast(`profile "${name}" created and selected`, true);
  loadEquipment();
});

action("eq-prof-rename", async () => {
  const sel = $("eq-profile");
  if (!sel.options.length) return;
  const oldName = sel.options[sel.selectedIndex].textContent;
  const name = prompt("New name for this profile:", oldName);
  if (!name || name === oldName) return;
  await rpc("rename_profile", { id: parseInt(sel.value, 10), new_name: name });
  toast("profile renamed", true);
  loadEquipment();
});

action("eq-prof-delete", async () => {
  const sel = $("eq-profile");
  if (!sel.options.length) return;
  const name = sel.options[sel.selectedIndex].textContent;
  if (!confirm(`Delete profile "${name}"?\n(Equipment must be disconnected.)`)) return;
  const darks = confirm("Also delete its dark library / bad-pixel-map files?\n(Cancel keeps the files.)");
  await rpc("delete_profile", { id: parseInt(sel.value, 10), delete_dark_files: darks });
  toast(`profile "${name}" deleted`, true);
  loadEquipment();
});

action("eq-save-profile", async () => {
  await rpc("set_profile_setup", {
    focal_length: parseInt($("eq-focal").value, 10) || 0,
    pixel_size: parseFloat($("eq-pixsize").value) || 0,
  });
  toast("profile setup saved", true);
  loadEquipment();
});

action("eq-save-alpaca", async () => {
  await rpc("set_alpaca_server", { host: $("eq-host").value, port: parseInt($("eq-port").value, 10) });
  toast("alpaca server saved", true);
  loadEquipment();
});

action("eq-discover", async () => {
  const out = $("eq-discover-out");
  out.textContent = "discovering…";
  const res = await fetch("/api/discover/alpaca").then((r) => r.json());
  const servers = res.servers || []; // ["host:port", ...]
  out.textContent = servers.length ? "found: " : "no Alpaca servers found";
  servers.forEach((s) => {
    const b = document.createElement("button");
    b.className = "btn";
    b.textContent = s;
    b.title = "use this server";
    b.addEventListener("click", () => {
      const i = s.lastIndexOf(":");
      $("eq-host").value = s.slice(0, i);
      $("eq-port").value = s.slice(i + 1);
    });
    out.appendChild(b);
  });
});

action("eq-connect", async () => {
  await rpc("set_selected_camera", { camera: $("eq-camera").value });
  await rpc("set_selected_mount", { mount: $("eq-mount").value });
  await rpc("set_selected_rotator", { rotator: $("eq-rotator").value });
  $("eq-status").textContent = "connecting…";
  try {
    await rpc("set_connected", { connected: true });
    $("eq-status").textContent = "";
    toast("equipment connected", true);
  } catch (e) {
    $("eq-status").textContent = "";
    throw e;
  }
  loadEquipment();
});
action("eq-disconnect", async () => { await rpc("set_connected", { connected: false }); loadEquipment(); });
action("eq-clear-cal", async () => { await rpc("clear_calibration", { which: "Both" }); loadEquipment(); });
action("eq-flip-cal", async () => { await rpc("flip_calibration"); toast("calibration flipped", true); });

/* ---------------- settings tab ---------------- */

async function loadAlgo(axis) {
  const algos = await rpcQuiet("get_algos");
  const cur = await rpcQuiet("get_algo", { axis });
  const sel = $("st-algo-" + axis);
  sel.innerHTML = "";
  (algos || []).forEach((a) => {
    const o = document.createElement("option");
    o.textContent = a;
    if (a === cur) o.selected = true;
    sel.appendChild(o);
  });
  await loadAlgoParams(axis);
}

async function loadAlgoParams(axis) {
  const box = $("st-params-" + axis);
  box.innerHTML = "";
  const names = await rpcQuiet("get_algo_param_names", { axis });
  if (!names) return;
  for (const name of names) {
    if (name === "algorithmName") continue;
    const val = await rpcQuiet("get_algo_param", { axis, name });
    if (val === undefined) continue;
    const label = document.createElement("label");
    label.innerHTML = `<span>${name}</span>`;
    const input = document.createElement("input");
    input.type = "number";
    input.step = "any";
    input.value = val;
    input.addEventListener("change", () =>
      rpc("set_algo_param", { axis, name, value: parseFloat(input.value) })
        .then(() => toast(`${axis} ${name} = ${input.value}`, true))
        .catch((e) => toast(e.message)));
    label.appendChild(input);
    box.appendChild(label);
  }
}

["ra", "dec"].forEach((axis) => {
  $("st-algo-" + axis).addEventListener("change", async () => {
    try {
      await rpc("set_algo", { axis, name: $("st-algo-" + axis).value });
      toast(`${axis} algorithm set`, true);
      await loadAlgoParams(axis);
    } catch (e) { toast(e.message); }
  });
});

async function loadSettings() {
  await loadAlgo("ra");
  await loadAlgo("dec");
  const decmode = await rpcQuiet("get_dec_guide_mode");
  if (decmode) $("st-decmode").value = decmode;
  const lim = await rpcQuiet("get_guide_limits");
  if (lim) { $("st-maxra").value = lim.MaxRaDuration; $("st-maxdec").value = lim.MaxDecDuration; }
  const dc = await rpcQuiet("get_dec_comp");
  if (dc !== undefined) $("st-deccomp").checked = dc;
  const out = await rpcQuiet("get_guide_output_enabled");
  if (out !== undefined) $("st-output").checked = out;
  const dith = await rpcQuiet("get_dither_settings");
  if (dith) { $("st-dscale").value = dith.ScaleFactor; $("st-dra").checked = dith.RaOnly; }
  const sd = await rpcQuiet("get_star_detection");
  if (sd) { $("st-minsnr").value = sd.MinStarSNR; $("st-minhfd").value = sd.MinStarHFD; $("st-maxhfd").value = sd.MaxStarHFD; }
  const gain = await rpcQuiet("get_camera_gain");
  if (gain) $("st-gain").value = gain.Gain;
  const to = await rpcQuiet("get_camera_timeout");
  if (to !== undefined) $("st-timeout").value = to;
  const nr = await rpcQuiet("get_noise_reduction");
  if (nr) $("st-noise").value = nr;
}

$("st-decmode").addEventListener("change", () =>
  rpc("set_dec_guide_mode", { mode: $("st-decmode").value }).catch((e) => toast(e.message)));

action("st-save-guiding", async () => {
  await rpc("set_guide_limits", {
    MaxRaDuration: parseInt($("st-maxra").value, 10),
    MaxDecDuration: parseInt($("st-maxdec").value, 10),
  });
  await rpc("set_dec_comp", { enabled: $("st-deccomp").checked });
  await rpc("set_guide_output_enabled", { enabled: $("st-output").checked });
  await rpc("set_dither_settings", { ScaleFactor: parseFloat($("st-dscale").value), RaOnly: $("st-dra").checked });
  toast("guiding settings applied", true);
});

action("st-save-star", async () => {
  await rpc("set_star_detection", {
    MinStarSNR: parseFloat($("st-minsnr").value),
    MinStarHFD: parseFloat($("st-minhfd").value),
    MaxStarHFD: parseFloat($("st-maxhfd").value),
  });
  toast("star detection applied", true);
});

action("st-save-camera", async () => {
  await rpc("set_camera_gain", { gain: parseInt($("st-gain").value, 10) });
  await rpc("set_camera_timeout", { timeout_ms: parseInt($("st-timeout").value, 10) });
  await rpc("set_noise_reduction", { method: $("st-noise").value });
  toast("camera settings applied", true);
});

/* ---------------- darks tab ---------------- */

const expLabel = (ms) => (ms < 1000 ? `${ms} ms` : `${+(ms / 1000).toFixed(2)} s`);
let darkExposures = [];

function darkBuildPlan() {
  if (!darkExposures.length) return;
  const lo = parseInt($("dk-min-exp").value, 10);
  const hi = parseInt($("dk-max-exp").value, 10);
  const frames = parseInt($("dk-frames").value, 10) || 0;
  if (lo > hi) {
    $("dk-build-plan").textContent = "min exposure is greater than max exposure";
    return;
  }
  const sel = darkExposures.filter((ms) => ms >= lo && ms <= hi);
  const totalS = sel.reduce((t, ms) => t + ms * frames, 0) / 1000;
  $("dk-build-plan").textContent =
    `will capture ${sel.length} master dark${sel.length === 1 ? "" : "s"} (${sel.map(expLabel).join(", ")}) ` +
    `× ${frames} frames each ≈ ${totalS < 90 ? Math.round(totalS) + " s" : Math.round(totalS / 60) + " min"} of exposure`;
}

async function loadDarkExposures() {
  if (darkExposures.length) return;
  darkExposures = ((await rpcQuiet("get_exposure_durations")) || []).sort((a, b) => a - b);
  const fill = (sel, def) => {
    sel.innerHTML = darkExposures.map((ms) => `<option value="${ms}">${expLabel(ms)}</option>`).join("");
    if (darkExposures.length)
      sel.value = darkExposures.includes(def) ? def : darkExposures[darkExposures.length - 1];
  };
  fill($("dk-min-exp"), 1000); // same defaults as the GUI's Build Dark Library dialog
  fill($("dk-max-exp"), 6000);
  darkBuildPlan();
}

["dk-min-exp", "dk-max-exp", "dk-frames"].forEach((id) => $(id).addEventListener("input", darkBuildPlan));

async function loadDarks() {
  loadDarkExposures();
  const st = await rpcQuiet("get_calibration_files_status");
  if (!st) return;
  const yn = (v) => (v ? '<b class="good">yes</b>' : "<b>no</b>");
  $("dk-status").innerHTML =
    `<span>dark library</span>${yn(st.dark_library_exists)}` +
    `<span>library loaded</span>${yn(st.dark_library_loaded)}` +
    `<span>bad-pixel map</span>${yn(st.defect_map_exists)}` +
    `<span>map loaded</span>${yn(st.defect_map_loaded)}` +
    (st.dark_count_loaded ? `<span>darks loaded</span><b>${st.dark_count_loaded}</b>` : "");
  $("dk-use-darks").checked = st.dark_library_loaded;
  $("dk-use-map").checked = st.defect_map_loaded;
  $("dk-auto-darks").checked = st.auto_load_darks;
  $("dk-auto-map").checked = st.auto_load_defect_map;
}

$("dk-use-darks").addEventListener("change", () =>
  rpc("set_dark_library_enabled", { enabled: $("dk-use-darks").checked }).then(loadDarks).catch((e) => { toast(e.message); loadDarks(); }));
$("dk-use-map").addEventListener("change", () =>
  rpc("set_defect_map_enabled", { enabled: $("dk-use-map").checked }).then(loadDarks).catch((e) => { toast(e.message); loadDarks(); }));
$("dk-auto-darks").addEventListener("change", () =>
  rpc("set_dark_auto_load", { auto_load_darks: $("dk-auto-darks").checked }).catch((e) => toast(e.message)));
$("dk-auto-map").addEventListener("change", () =>
  rpc("set_dark_auto_load", { auto_load_defect_map: $("dk-auto-map").checked }).catch((e) => toast(e.message)));

$("dk-hot").addEventListener("input", () => { $("dk-hot-val").textContent = $("dk-hot").value; });
$("dk-cold").addEventListener("input", () => { $("dk-cold-val").textContent = $("dk-cold").value; });

// Poll get_dark_build_progress while a (synchronous) build RPC is in
// flight and render it into a <progress> bar + message line. Returns a
// stop function that hides the bar and ends the polling.
function trackDarkBuild(barId, msgEl) {
  const bar = $(barId);
  bar.hidden = false;
  bar.removeAttribute("value"); // indeterminate until the first poll lands
  let stopped = false;
  const timer = setInterval(async () => {
    const p = await rpcQuiet("get_dark_build_progress");
    if (stopped || !p || !p.active) return;
    const total = p.exposure_count * p.frame_count;
    const done = (p.exposure_index - 1) * p.frame_count + (p.frame - 1);
    bar.max = total;
    bar.value = done;
    msgEl.textContent =
      `capturing dark ${Math.min(done + 1, total)}/${total} — ` +
      `exposure ${p.exposure_index}/${p.exposure_count} (${expLabel(p.exposure_ms)}), frame ${p.frame}/${p.frame_count}`;
  }, 700);
  return () => {
    stopped = true;
    clearInterval(timer);
    bar.hidden = true;
  };
}

action("dk-build", async () => {
  const lo = parseInt($("dk-min-exp").value, 10);
  const hi = parseInt($("dk-max-exp").value, 10);
  if (lo > hi) {
    toast("min exposure is greater than max exposure");
    return;
  }
  $("dk-build-msg").textContent = "building dark library…";
  const stop = trackDarkBuild("dk-build-bar", $("dk-build-msg"));
  try {
    const r = await rpc("build_dark_library", {
      min_exposure_ms: lo,
      max_exposure_ms: hi,
      frame_count: parseInt($("dk-frames").value, 10),
      clear_existing: $("dk-clear").checked,
    });
    stop();
    $("dk-build-msg").textContent = `built ${r.exposure_count} exposures × ${r.frame_count} frames (${(r.exposures_ms || []).map(expLabel).join(", ")})`;
    loadDarks();
  } catch (e) {
    stop();
    $("dk-build-msg").textContent = "";
    throw e;
  }
});

action("dk-bpm-build", async () => {
  $("dk-bpm-msg").textContent = "capturing defect-map darks…";
  const stop = trackDarkBuild("dk-bpm-bar", $("dk-bpm-msg"));
  try {
    const r = await rpc("build_defect_map_darks", {
      exposure_ms: parseInt($("dk-bpm-exp").value, 10),
      frame_count: parseInt($("dk-bpm-frames").value, 10),
    });
    stop();
    $("dk-bpm-msg").textContent = `map built: ${r.defect_count} bad pixels`;
    loadDarks();
  } catch (e) {
    stop();
    $("dk-bpm-msg").textContent = "";
    throw e;
  }
});

const rebuildParams = (save) => ({
  aggressiveness_hot: parseInt($("dk-hot").value, 10),
  aggressiveness_cold: parseInt($("dk-cold").value, 10),
  save,
});
action("dk-preview", async () => {
  const r = await rpc("rebuild_defect_map", rebuildParams(false));
  $("dk-bpm-msg").textContent = `would flag ${r.hot_pixel_count} hot + ${r.cold_pixel_count} cold = ${r.defect_count} pixels`;
});
action("dk-rebuild", async () => {
  const r = await rpc("rebuild_defect_map", rebuildParams(true));
  $("dk-bpm-msg").textContent = `map rebuilt: ${r.defect_count} pixels (${r.hot_pixel_count} hot, ${r.cold_pixel_count} cold)`;
  loadDarks();
});

action("dk-delete", async () => {
  if (!confirm("Delete the dark library and bad-pixel map files?")) return;
  await rpc("delete_calibration_files", { delete_dark_library: true, delete_defect_map: true });
  toast("calibration files deleted", true);
  loadDarks();
});

/* ---------------- shutdown ---------------- */

let shutDown = false;

action("btn-shutdown", async () => {
  if (!confirm("Shut down the guider daemon?\n\nGuiding and looping will stop, equipment stays as-is, and this page " +
               "will be unreachable until the daemon is started again on the guider machine."))
    return;
  shutDown = true;
  try {
    await rpc("shutdown");
  } catch (e) {
    // The daemon can drop the connection while exiting — that's still success.
  }
  $("pill-state").textContent = "shut down";
  $("pill-state").className = "pill";
  document.querySelector("main").innerHTML =
    '<div class="card"><div class="card-title"><span>Daemon shut down</span></div>' +
    "<p>The guider daemon has exited. Start it again on the guider machine, then reload this page.</p></div>";
});

/* ---------------- main loop ---------------- */

async function tick() {
  if (shutDown) return;
  await pollHeader();
  if (activeTab === "guide") await pollGuide();
  if (activeTab === "polar") {
    if (activePa === "staticpa") await pollStaticPa();
    if (activePa === "polardrift") await pollPolarDrift();
    if (activePa === "driftalign") await pollDriftAlign();
  }
}

(async function init() {
  const v = await rpcQuiet("get_version");
  if (v && v.version) $("header-version").textContent = "v" + v.version;
  await loadExposures();
  await tick();
  setInterval(tick, 1200);
})();
