import { LANGUAGES, applyStatic, has, lang, setLang, t, tError } from "./i18n.js";

const $ = (sel) => document.querySelector(sel);

const state = {
  printers: [],
  files: [],
  folders: [],
  file: null,      // library entry being viewed
  info: null,      // /api/file/info of that file
  tab: "3d",
  follow: null,    // printer id whose progress drives the slider
};

let viewer = null;          // Viewer3D, loaded lazily
let viewerFailed = false;
let voxelToken = 0;

// --- helpers --------------------------------------------------------------------

class ApiError extends Error {
  constructor(data) {
    super(typeof data === "string" ? data : data.error);
    this.data = data;
  }
}

async function api(path, opts = {}) {
  const res = await fetch(path, {
    ...opts,
    headers: opts.body && !(opts.body instanceof FormData) ? { "Content-Type": "application/json" } : {},
    body: opts.body && !(opts.body instanceof FormData) ? JSON.stringify(opts.body) : opts.body,
  });
  const type = res.headers.get("content-type") || "";
  const data = type.includes("json") ? await res.json() : await res.text();
  if (res.status === 401 && data?.code === "pin_required") showLogin();
  if (!res.ok) throw new ApiError(data || res.statusText);
  return data;
}

const errText = (e) => (e instanceof ApiError ? tError(e.data) : e.message);

function toast(msg, kind = "") {
  const el = document.createElement("div");
  el.className = `toast ${kind}`;
  el.textContent = msg;
  $("#toasts").append(el);
  setTimeout(() => el.remove(), kind === "error" ? 8000 : 4000);
}

function el(tag, attrs = {}, ...children) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (k.startsWith("on")) e.addEventListener(k.slice(2), v);
    else if (k === "class") e.className = v;
    else if (v !== false && v != null) e.setAttribute(k, v === true ? "" : v);
  }
  e.append(...children.flat().filter((c) => c != null && c !== false));
  return e;
}

const fmtTime = (s) => {
  s = Math.round(+s || 0);
  const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60);
  return h ? `${h}h${String(m).padStart(2, "0")}` : `${m} min`;
};
const fmtSize = (b) => (b > 1e6 ? `${(b / 1e6).toFixed(1)} MB` : `${Math.round(b / 1e3)} KB`);
const fmtDate = (ts) => new Date(ts * 1000).toLocaleString(lang, { dateStyle: "short", timeStyle: "short" });
const q = (path) => `path=${encodeURIComponent(path)}`;

function confirmDialog(title, text, okLabel = t("dialog.confirm"), danger = false) {
  const d = $("#confirm-dialog");
  $("#confirm-title").textContent = title;
  $("#confirm-text").textContent = text;
  const ok = $("#confirm-ok");
  ok.textContent = okLabel;
  ok.className = danger ? "danger" : "primary";
  d.returnValue = "";
  d.showModal();
  return new Promise((resolve) => d.addEventListener("close", () => resolve(d.returnValue === "ok"), { once: true }));
}

// --- printers -------------------------------------------------------------------

// printStatus values seen in HALOT BOX and HalotMageProSharp -> dot style
const STATUS_CLASS = {
  PRINT_GENERAL: "online",
  PRINT_PROCESSING: "busy",
  PRINT_PROGRESS: "busy",
  PRINT_PRINTING: "busy",
  PRINT_PAUSE: "paused",
  PRINT_PAUSING: "paused",
  PRINT_STOP: "online",
  PRINT_STOPING: "paused",
  PRINT_STOPPING: "paused",
  PRINT_COMPLETING: "busy",
  PRINT_COMPLETE: "online",
  PRINT_END: "online",
  PRINT_FAIL: "offline",
};
const IDLE = new Set(["", "PRINT_GENERAL", "PRINT_COMPLETE", "PRINT_END", "PRINT_STOP", "PRINT_FAIL"]);

function printerView(p) {
  const st = p.status?.printStatus || "";
  let label = !st ? t("status.online")
    : has(`status.${st}`) ? t(`status.${st}`) : st.replace(/^PRINT_/, "").toLowerCase();
  let cls = STATUS_CLASS[st] || "online";
  if (!p.online) [label, cls] = [t("status.offline"), "offline"];
  const total = +p.status?.sliceLayerCount || 0;
  const cur = +p.status?.curSliceLayer || 0;
  const active = p.online && !IDLE.has(st) && total > 0;
  const paused = st.includes("PAUSE");
  return { label, cls, total, cur, active, paused, st };
}

function renderPrinters() {
  const box = $("#printers");
  box.replaceChildren();
  if (!state.printers.length) {
    box.append(el("div", { class: "empty-note" }, t("printers.none")));
  }
  for (const p of state.printers) {
    const v = printerView(p);
    const fname = p.status?.filename;
    const local = fname && state.files.find((f) => f.name === fname);
    const rem = +p.status?.printRemainTime || 0;
    const card = el("div", { class: "printer" },
      el("div", { class: "printer-top" },
        el("span", { class: `dot ${v.cls}` }),
        el("span", { class: "printer-name" }, p.name),
        el("span", { class: "printer-host" }, p.host)),
      el("div", { class: "printer-state" }, v.label, p.version ? el("span", { class: "muted small" }, `  · fw ${p.version}`) : null),
      fname ? el("div", { class: "printer-file", title: fname },
        local ? el("a", { onclick: () => openFile(local, p.id) }, fname) : fname) : null,
      v.total ? [
        el("div", { class: "progress" }, el("div", { style: `width:${(v.cur * 100) / v.total}%` })),
        el("div", { class: "progress-text" },
          el("span", {}, t("printers.layer", { cur: v.cur, total: v.total, pct: Math.floor((v.cur * 100) / v.total) })),
          el("span", {}, rem ? t("printers.remaining", { time: fmtTime(rem) }) : "")),
      ] : null,
      p.upload ? [
        el("div", { class: "progress upload" }, el("div", { style: `width:${p.upload.pct}%` })),
        el("div", { class: "progress-text" },
          el("span", {}, t("printers.uploading", { file: p.upload.file })), el("span", {}, `${p.upload.pct}%`)),
      ] : null,
      p.error ? el("div", { class: "printer-error" }, tError(p.error)) : null,
      el("div", { class: "printer-actions" },
        v.active ? [
          el("button", { class: "small", onclick: () => printerAction(p, "pause") },
            v.paused ? t("printers.resume") : t("printers.pause")),
          el("button", { class: "small danger", onclick: () => printerAction(p, "stop") }, t("printers.stop")),
        ] : null,
        v.active && local && state.follow !== p.id
          ? el("button", { class: "small", onclick: () => openFile(local, p.id) }, t("printers.follow")) : null,
        el("button", { class: "ghost small", title: t("printers.configureTitle"), onclick: () => openPrinterDialog(p) },
          t("printers.configure"))),
    );
    box.append(card);
  }
  renderTargets();
  updateFollow();
}

function renderTargets() {
  const sel = $("#target");
  const prev = sel.value;
  sel.replaceChildren(...state.printers.map((p) =>
    el("option", { value: p.id }, `${p.name}${p.online ? "" : ` (${t("status.offline").toLowerCase()})`}`)));
  if (state.printers.some((p) => p.id === prev)) sel.value = prev;
  updateSendButtons();
}

function updateSendButtons() {
  const p = state.printers.find((x) => x.id === $("#target").value);
  const ok = p && p.online && p.hasPassword && !p.upload && IDLE.has(p.status?.printStatus || "");
  $("#send").disabled = $("#send-print").disabled = !ok || !state.file;
  const why = !p ? t("send.whyNoPrinter") : !p.online ? t("send.whyOffline")
    : !p.hasPassword ? t("send.whyPassword") : p.upload ? t("send.whyUploading")
    : !IDLE.has(p.status?.printStatus || "") ? t("send.whyBusy") : "";
  $("#send").title = $("#send-print").title = why;
}

async function printerAction(p, action) {
  const v = printerView(p);
  if (action === "stop" && !(await confirmDialog(t("stop.title"),
    t("stop.text", { file: p.status?.filename || "", printer: p.name }), t("printers.stop"), true))) return;
  try {
    await api(`/api/printers/${p.id}/${action}`, { method: "POST" });
    toast(action === "stop" ? t("stop.done") : v.paused ? t("pause.resuming") : t("pause.pausing"), "ok");
  } catch (e) {
    toast(errText(e), "error");
  }
}

let editing = null;
function openPrinterDialog(p = null) {
  editing = p;
  const f = $("#printer-form");
  f.reset();
  $("#printer-dialog-title").textContent = p ? t("printerDialog.edit") : t("printerDialog.add");
  f.name.value = p?.name || "";
  f.host.value = p?.host || "";
  $("#pw-hint").hidden = !p?.hasPassword;
  $("#printer-delete").hidden = !p;
  $("#printer-error").textContent = "";
  $("#scan-results").replaceChildren();
  $("#printer-dialog").showModal();
}

$("#add-printer").onclick = () => openPrinterDialog();
$("#printer-save").onclick = async (ev) => {
  ev.preventDefault();
  const f = $("#printer-form");
  if (!f.reportValidity()) return;
  const body = { name: f.name.value, host: f.host.value, password: f.password.value };
  try {
    if (editing) await api(`/api/printers/${editing.id}`, { method: "PUT", body });
    else await api("/api/printers", { method: "POST", body });
    $("#printer-dialog").close();
  } catch (e) {
    $("#printer-error").textContent = errText(e);
  }
};
$("#printer-delete").onclick = async () => {
  const p = editing;
  $("#printer-dialog").close();
  if (await confirmDialog(t("remove.title"), t("remove.text", { name: p.name, host: p.host }),
    t("printerDialog.remove"), true)) {
    await api(`/api/printers/${p.id}`, { method: "DELETE" }).catch((e) => toast(errText(e), "error"));
  }
};
$("#scan").onclick = async () => {
  const btn = $("#scan"), out = $("#scan-results");
  btn.disabled = true;
  out.replaceChildren(t("printerDialog.scanning"));
  try {
    const found = await api("/api/printers/scan", { method: "POST" });
    out.replaceChildren(...(found.length ? found.map((r) =>
      el("button", { type: "button", class: "small", onclick: () => { $("#printer-form").host.value = r.host; } },
        r.host + (r.known ? t("printerDialog.scanKnown") : ""))) : [t("printerDialog.scanNone")]));
  } catch (e) {
    out.replaceChildren(errText(e));
  } finally {
    btn.disabled = false;
  }
};

// --- library --------------------------------------------------------------------

async function loadLibrary() {
  const data = await api("/api/library");
  state.files = data.files;
  state.folders = data.folders;
  renderLibrary();
  renderPrinters();
}

function renderLibrary() {
  const filter = $("#filter").value.toLowerCase();
  const box = $("#library");
  box.replaceChildren();
  if (!state.folders.length) {
    box.append(el("div", { class: "empty-note" }, t("library.noFolder")));
    return;
  }
  const files = state.files.filter((f) => f.name.toLowerCase().includes(filter));
  if (!files.length) box.append(el("div", { class: "empty-note" }, t("library.empty")));
  for (const f of files) {
    box.append(el("div", {
      class: `file${state.file?.path === f.path ? " active" : ""}`,
      title: f.path,
      onclick: () => openFile(f),
    },
      el("img", { src: `/api/file/preview?size=small&${q(f.path)}&t=${f.mtime}`, loading: "lazy", alt: "" }),
      el("div", { class: "file-info" },
        el("div", { class: "file-title" }, f.name),
        el("div", { class: "file-sub" }, `${fmtSize(f.size)} · ${fmtDate(f.mtime)}`))));
  }
}

$("#filter").oninput = renderLibrary;

$("#edit-folders").onclick = () => {
  const f = $("#folders-form");
  f.folders.value = state.folders.join("\n");
  $("#folders-error").textContent = "";
  $("#folders-dialog").showModal();
};
$("#folders-form").addEventListener("submit", async (ev) => {
  if (ev.submitter?.value !== "save") return;
  ev.preventDefault();
  const folders = ev.target.folders.value.split("\n").map((s) => s.trim()).filter(Boolean);
  try {
    await api("/api/library/folders", { method: "PUT", body: { folders } });
    $("#folders-dialog").close();
    loadLibrary();
  } catch (e) {
    $("#folders-error").textContent = errText(e);
  }
});

async function uploadFiles(files) {
  const form = new FormData();
  for (const f of files) form.append("file", f, f.name);
  try {
    toast(t("library.adding", { n: files.length }));
    const r = await api("/api/library/upload", { method: "POST", body: form });
    await loadLibrary();
    const added = state.files.find((f) => f.path === r.saved[0]);
    if (added) openFile(added);
  } catch (e) {
    toast(errText(e), "error");
  }
}
$("#upload").onchange = (e) => e.target.files.length && uploadFiles([...e.target.files]);
let dragDepth = 0;
window.addEventListener("dragenter", (e) => { e.preventDefault(); dragDepth++; document.body.classList.add("dragging"); });
window.addEventListener("dragleave", () => { if (--dragDepth <= 0) document.body.classList.remove("dragging"); });
window.addEventListener("dragover", (e) => e.preventDefault());
window.addEventListener("drop", (e) => {
  e.preventDefault();
  dragDepth = 0;
  document.body.classList.remove("dragging");
  const files = [...e.dataTransfer.files].filter((f) => /\.cxdlp(v4)?$/i.test(f.name));
  if (files.length) uploadFiles(files);
  else toast(t("library.dropOnly"), "error");
});

// --- viewer ---------------------------------------------------------------------

async function openFile(f, follow = null) {
  state.file = f;
  history.replaceState(null, "", `#file=${encodeURIComponent(f.name)}`);
  state.follow = follow;
  $("#empty").hidden = true;
  $("#file-view").hidden = false;
  $("#file-name").textContent = f.name;
  $("#file-meta").textContent = `${fmtSize(f.size)} · ${f.folder}`;
  $("#stats").replaceChildren();
  $("#thumb-img").src = `/api/file/preview?${q(f.path)}&t=${f.mtime}`;
  // on a phone the viewer sits below the printers and the library
  if (matchMedia("(max-width: 820px)").matches) $("#viewer").scrollIntoView({ behavior: "smooth" });
  renderLibrary();
  updateSendButtons();
  try {
    const info = await api(`/api/file/info?${q(f.path)}`);
    if (state.file !== f) return;
    state.info = info;
    renderStats();
    const slider = $("#layer");
    slider.max = info.layerCount - 1;
    slider.value = info.layerCount - 1;
    load3D(f);
    updateFollow();
    if (!state.follow) setLayer(info.layerCount - 1);
  } catch (e) {
    toast(t("viewer.readFail", { error: errText(e) }), "error");
  }
}

function renderStats() {
  const i = state.info;
  if (!i) return;
  const items = [
    [t("stats.printer"), i.printer],
    [t("stats.time"), fmtTime(i.printTime)],
    [t("stats.layers"), `${i.layerCount} × ${i.layerHeight} mm`],
    [t("stats.height"), `${i.printHeight} mm`],
    [t("stats.resin"), i.volumeMl ? `${i.volumeMl.toFixed(1)} ml` : "—"],
    [t("stats.exposure"), t("stats.exposureValue", { exp: i.exposure, bottom: i.bottomExposure, n: i.bottomLayers })],
    [t("stats.antialias"), `${i.antiAlias}×`],
  ];
  $("#stats").replaceChildren(...items.map(([k, v]) => el("div", {}, el("dt", {}, k), el("dd", {}, v))));
}

async function ensureViewer() {
  if (viewer || viewerFailed) return viewer;
  try {
    const { Viewer3D } = await import("./viewer3d.js");
    viewer = new Viewer3D($("#three"));
  } catch (e) {
    viewerFailed = true;
    console.error(e);
  }
  return viewer;
}

async function load3D(f) {
  const token = ++voxelToken;
  const status = $("#vox-status");
  const v = await ensureViewer();
  if (!v) {
    status.textContent = t("viewer.no3d");
    return;
  }
  v.clear();
  status.textContent = state.info?.hasVoxels ? t("viewer.loading3d") : t("viewer.building3d");
  try {
    const res = await fetch(`/api/file/voxels?${q(f.path)}`);
    if (!res.ok) throw new Error(await res.text());
    const buf = await res.arrayBuffer();
    if (token !== voxelToken) return;
    status.textContent = t("viewer.meshing");
    await new Promise((r) => setTimeout(r, 20));
    v.load(buf);
    status.textContent = "";
    applyCut();
  } catch (e) {
    if (token === voxelToken) status.textContent = t("viewer.fail3d", { error: e.message });
  }
}

function setLayer(n) {
  const info = state.info;
  if (!info) return;
  n = Math.max(0, Math.min(info.layerCount - 1, n));
  $("#layer").value = n;
  $("#layer-label").textContent = `${n + 1}/${info.layerCount} · ${info.layers[n].toFixed(2)} mm`;
  if (state.tab === "layers") showLayerImage(n);
  applyCut();
}

let layerTimer = null;
function showLayerImage(n) {
  clearTimeout(layerTimer);
  layerTimer = setTimeout(() => {
    $("#layer-img").src = `/api/file/layer?${q(state.file.path)}&n=${n}&t=${state.file.mtime}`;
  }, 60);
}

function applyCut() {
  if (!viewer || !state.info) return;
  const n = +$("#layer").value;
  const last = n >= state.info.layerCount - 1;
  viewer.setCut(last ? null : state.info.layers[n]);
}

$("#layer").oninput = (e) => {
  state.follow = null;
  updateFollow();
  setLayer(+e.target.value);
};

for (const btn of document.querySelectorAll(".tabs button")) {
  btn.onclick = () => {
    state.tab = btn.dataset.tab;
    for (const b of document.querySelectorAll(".tabs button")) b.classList.toggle("active", b === btn);
    for (const tab of ["3d", "layers", "thumb"]) $(`#tab-${tab}`).hidden = tab !== state.tab;
    if (state.tab === "layers") showLayerImage(+$("#layer").value);
    if (state.tab === "3d") viewer?.resize();
  };
}

// keep the slider on the layer being printed while following a printer
function updateFollow() {
  const badge = $("#follow-badge");
  const p = state.printers.find((x) => x.id === state.follow);
  const v = p && printerView(p);
  if (!p || !state.info || p.status?.filename !== state.file?.name || !v.active) {
    badge.hidden = true;
    return;
  }
  badge.hidden = false;
  badge.textContent = t(v.paused ? "viewer.followingPaused" : "viewer.following",
    { name: p.name, cur: v.cur, total: v.total });
  setLayer(Math.max(0, v.cur - 1));
}

// --- send -----------------------------------------------------------------------

async function send(start) {
  const p = state.printers.find((x) => x.id === $("#target").value);
  const f = state.file;
  if (!p || !f) return;
  if (start && !(await confirmDialog(t("send.confirmTitle"),
    t("send.confirmText", { file: f.name, printer: p.name }), t("send.sendPrint")))) return;
  try {
    await api(`/api/printers/${p.id}/send`, { method: "POST", body: { path: f.path, print: start } });
    toast(start ? t("send.sentPrinting", { printer: p.name }) : t("send.sent", { printer: p.name }), "ok");
    if (start) state.follow = p.id;
  } catch (e) {
    toast(errText(e), "error");
  }
}
$("#send").onclick = () => send(false);
$("#send-print").onclick = () => send(true);
$("#target").onchange = updateSendButtons;

// --- language -------------------------------------------------------------------

function initLanguage() {
  const sel = $("#lang");
  const names = { en: "EN", "pt-BR": "PT" };
  sel.replaceChildren(...LANGUAGES.map((l) => el("option", { value: l }, names[l] || l)));
  sel.value = lang;
  sel.onchange = () => {
    setLang(sel.value);
    renderPrinters();
    renderLibrary();
    renderStats();
    if (state.info) setLayer(+$("#layer").value);
  };
  setLang(lang);
}

// --- access from the phone ------------------------------------------------------

async function openRemoteDialog() {
  const r = await api("/api/remote").catch((e) => (toast(errText(e), "error"), null));
  if (!r) return;
  $("#remote-on").hidden = !r.enabled;
  $("#remote-off").hidden = r.enabled;
  if (r.enabled) {
    if (!$("#remote-dialog").open) $("#remote-qr").src = `/api/remote/qr.svg?t=${Date.now()}`;
    $("#remote-urls").replaceChildren(...r.urls.map((u) => el("a", { href: u, target: "_blank" }, u)));
    $("#remote-pin").textContent = r.pin;
    renderDevices(r.devices);
  }
  if (!$("#remote-dialog").open) $("#remote-dialog").showModal();
}

function renderDevices(devices) {
  const box = $("#remote-devices");
  if (!devices.length) {
    box.replaceChildren(el("div", { class: "empty-note" }, t("remote.noDevices")));
    return;
  }
  box.replaceChildren(...devices.map((d) => el("div", { class: "device" },
    el("span", { class: `dot ${d.online ? "online" : ""}` }),
    el("div", { class: "device-info" },
      el("div", {}, d.name),
      el("div", { class: "device-sub" }, [d.ip,
        d.online ? t("remote.online") : d.seen ? t("remote.seen", { date: fmtDate(d.seen) })
          : t("remote.since", { date: fmtDate(d.created) })].join(" · "))),
    el("button", { type: "button", class: "ghost small danger", onclick: async () => {
      try {
        await api(`/api/remote/devices/${d.id}`, { method: "DELETE" });
        toast(t("remote.disconnected", { name: d.name }), "ok");
      } catch (e) {
        toast(errText(e), "error");
      }
    } }, t("remote.disconnect")))));
}
$("#remote-btn").onclick = openRemoteDialog;

let quitting = false;
$("#quit-btn").onclick = async () => {
  if (!(await confirmDialog(t("quit.button"), t("quit.confirm"), t("quit.button"), true))) return;
  try {
    await api("/api/quit", { method: "POST" });
    quitting = true;
    document.body.replaceChildren(el("div", { class: "login" }, el("p", { class: "muted" }, t("quit.done"))));
  } catch (e) {
    toast(errText(e), "error");
  }
};
$("#remote-new-pin").onclick = async () => {
  try {
    const r = await api("/api/remote/pin", { method: "POST" });
    $("#remote-pin").textContent = r.pin;
    $("#remote-qr").src = `/api/remote/qr.svg?t=${Date.now()}`;
    renderDevices([]);
    toast(t("remote.newPinDone"), "ok");
  } catch (e) {
    toast(errText(e), "error");
  }
};

function showLogin() {
  if (!$("#login").hidden) return;
  $("#login").hidden = false;
  $("#login-form").pin.focus();
}

async function login(pin) {
  try {
    await api("/api/login", { method: "POST", body: { pin } });
    location.reload();
  } catch (e) {
    $("#login-error").textContent = errText(e);
  }
}
$("#login-form").addEventListener("submit", (ev) => {
  ev.preventDefault();
  login(ev.target.pin.value.trim());
});

// --- live updates ---------------------------------------------------------------

function connect() {
  const ws = new WebSocket(`${location.protocol === "https:" ? "wss" : "ws"}://${location.host}/ws`);
  ws.onopen = () => $("#conn").classList.add("on");
  ws.onclose = () => {
    $("#conn").classList.remove("on");
    if (quitting || !$("#login").hidden) return;
    // a phone that was disconnected from the computer goes back to the PIN screen
    setTimeout(() => api("/api/session")
      .then((s) => (s.authorized ? connect() : showLogin()))
      .catch(() => setTimeout(connect, 2000)), 1000);
  };
  ws.onmessage = (ev) => {
    const msg = JSON.parse(ev.data);
    if (msg.type === "printers") {
      state.printers = msg.printers;
      renderPrinters();
    } else if (msg.type === "devices" && $("#remote-dialog").open) {
      openRemoteDialog();
    } else if (msg.type === "voxels" && msg.path === state.file?.path) {
      $("#vox-status").textContent = t("viewer.building3dPct", { pct: Math.round(msg.pct * 100) });
    }
  };
}

async function start() {
  initLanguage();
  applyStatic();
  const hash = new URLSearchParams(location.hash.slice(1));
  const session = await api("/api/session");
  if (!session.authorized) return showLogin();
  $("#remote-btn").hidden = $("#quit-btn").hidden = !session.local;
  connect();
  await loadLibrary();
  const path = hash.get("file");
  const f = path && state.files.find((x) => x.path === path || x.name === path);
  if (f) openFile(f);
}

start().catch((e) => toast(errText(e), "error"));
