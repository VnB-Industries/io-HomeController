"use strict";

// Keep identical to CLOUD_MQTT_TOPIC_PREFIX in src/cloud_mqtt.cpp.
const TOPIC_PREFIX = "home/esp32lora";
const TOPIC_CMD = `${TOPIC_PREFIX}/cmd`;
const TOPIC_ACK = `${TOPIC_PREFIX}/ack`;
const TOPIC_STATE = `${TOPIC_PREFIX}/state`;
const TOPIC_STATUS = `${TOPIC_PREFIX}/status`;

const BROKER_URL = "wss://mqtt.flespi.io:443";
const STORAGE_KEY = "volets.config";
const ACK_TIMEOUT_MS = 5000;
const RECONNECT_AFTER_HIDDEN_MS = 5000;

const ERROR_LABELS = {
  "Bad signature": "Clé de commande incorrecte",
  "Stale command": "Heure de l'iPhone décalée",
  "Unknown device": "Volet inconnu",
  "Invalid action": "Commande refusée",
  "Invalid name": "Nom invalide",
  "Invalid seconds": "Temps de course invalide",
  "Invalid percent": "Position invalide",
  "Invalid room": "Nom de pièce invalide"
};

const elements = Object.fromEntries(
  ["connection", "open-settings", "devices-view", "devices", "empty", "drag-hint", "settings-view", "settings-error",
    "settings-form", "token", "command-key", "close-settings", "logout",
    "device-dialog", "device-form", "device-name", "device-room", "room-list", "device-time", "device-repeat", "device-error",
    "device-cancel"].map((id) => [id, document.getElementById(id)])
);

let config = loadConfig();
let client = null;
let signingKey = null;
let brokerConnected = false;
let boardOnline = false;
let devices = [];
let editedDeviceId = null;
let hiddenSince = 0;
const pending = new Map();
const deviceStatus = new Map();

function loadConfig() {
  try {
    return JSON.parse(localStorage.getItem(STORAGE_KEY));
  } catch {
    return null;
  }
}

function saveConfig(value) {
  try {
    localStorage.setItem(STORAGE_KEY, JSON.stringify(value));
  } catch {
    // Private mode: the settings still work until the app is closed.
  }
}

function clearConfig() {
  try {
    localStorage.removeItem(STORAGE_KEY);
  } catch {
    // Nothing stored.
  }
}

// Text covered by the signature. Same construction as signedText() in src/cloud_mqtt.cpp.
function signedText(id, ts, action, params) {
  let value = "";
  if (Number.isInteger(params.seconds)) {
    value = String(params.seconds);
  } else if (typeof params.enabled === "boolean") {
    value = params.enabled ? "1" : "0";
  } else if (typeof params.name === "string") {
    value = params.name;
  } else if (Number.isInteger(params.percent)) {
    value = String(params.percent);
  } else if (typeof params.room === "string") {
    value = params.room;
  }
  return [id, ts, action, params.deviceId || "", value].join("\n");
}

async function sign(text) {
  const mac = await crypto.subtle.sign("HMAC", signingKey, new TextEncoder().encode(text));
  return Array.from(new Uint8Array(mac), (byte) => byte.toString(16).padStart(2, "0")).join("");
}

async function importSigningKey(key) {
  return crypto.subtle.importKey("raw", new TextEncoder().encode(key), { name: "HMAC", hash: "SHA-256" }, false, ["sign"]);
}

// Resolves with null on success, or with the text to show on failure.
async function sendCommand(action, params) {
  const id = `c-${Date.now()}-${Math.floor(Math.random() * 1e6)}`;
  const ts = Math.floor(Date.now() / 1000);
  const sig = await sign(signedText(id, ts, action, params));
  return new Promise((resolve) => {
    const timer = setTimeout(() => {
      pending.delete(id);
      resolve("Pas de réponse");
    }, ACK_TIMEOUT_MS);
    pending.set(id, { timer, resolve });
    client.publish(TOPIC_CMD, JSON.stringify({ id, action, params, ts, sig }), { qos: 1 });
  });
}

function handleAck(ack) {
  const entry = pending.get(ack.id);
  if (!entry) {
    return;
  }
  pending.delete(ack.id);
  clearTimeout(entry.timer);
  entry.resolve(ack.ok ? null : (ERROR_LABELS[ack.error] || ack.error || "Erreur"));
}

async function runButton(deviceId, action) {
  setDeviceStatus(deviceId, "En cours…", "pending");
  const error = await sendCommand(action, { deviceId });
  if (error) {
    setDeviceStatus(deviceId, error, "error");
  } else {
    setDeviceStatus(deviceId, "Commande envoyée", "ok");
  }
}

async function runPosition(deviceId, percent) {
  setDeviceStatus(deviceId, "En cours…", "pending");
  const error = await sendCommand("position", { deviceId, percent });
  if (error) {
    targets.delete(deviceId);
    renderDevices();
    setDeviceStatus(deviceId, error, "error");
  } else {
    setDeviceStatus(deviceId, `Position ${percent} % envoyée`, "ok");
  }
}

function setDeviceStatus(deviceId, text, kind) {
  deviceStatus.set(deviceId, { text, kind });
  const line = elements.devices.querySelector(`[data-id="${CSS.escape(deviceId)}"] .device-status`);
  if (line) {
    line.textContent = text;
    line.className = `device-status ${kind}`;
  }
}

const SVG_NS = "http://www.w3.org/2000/svg";
const ICON_PATHS = {
  open: "M6 15l6-6 6 6",
  stop: "M8 8h8v8H8z",
  close: "M6 9l6 6 6-6",
  edit: "M4 20h4L19 9l-4-4L4 16v4z"
};
const STEP = 5;
const KEY_SEND_DELAY_MS = 700;
// A requested position stays drawn until the estimate reaches it, or this long after the travel time.
const TARGET_GRACE_MS = 5000;
const targets = new Map();

function icon(name) {
  const svg = document.createElementNS(SVG_NS, "svg");
  svg.setAttribute("viewBox", "0 0 24 24");
  svg.setAttribute("aria-hidden", "true");
  svg.classList.add("icon", `icon-${name}`);
  const path = document.createElementNS(SVG_NS, "path");
  path.setAttribute("d", ICON_PATHS[name]);
  svg.append(path);
  return svg;
}

function button(content, ariaLabel, onClick) {
  const element = document.createElement("button");
  element.type = "button";
  element.append(content);
  element.setAttribute("aria-label", ariaLabel);
  element.dataset.command = "true";
  element.addEventListener("click", onClick);
  return element;
}

function commandsEnabled() {
  return brokerConnected && boardOnline;
}

function snap(percent) {
  return Math.min(100, Math.max(0, Math.round(percent / STEP) * STEP));
}

function findItem(deviceId) {
  return elements.devices.querySelector(`[data-id="${CSS.escape(deviceId)}"]`);
}

// Draws the amber line and shows the value in the reading; null hides both.
function showTarget(item, percent, preview) {
  item.classList.toggle("has-target", percent !== null);
  item.classList.toggle("is-preview", preview);
  if (percent !== null) {
    item.querySelector(".window").style.setProperty("--target", String((100 - percent) / 100));
  }
}

function renderDevices() {
  elements.empty.hidden = devices.length > 0;
  elements["drag-hint"].hidden = devices.length === 0;
  const seen = new Set();
  for (const device of devices) {
    seen.add(device.id);
    let item = findItem(device.id);
    if (!item) {
      item = createDeviceItem(device.id);
      elements.devices.appendChild(item);
    }
    const position = Math.round(device.position);
    const pane = item.querySelector(".window");
    pane.style.setProperty("--closed", String((100 - device.position) / 100));
    pane.setAttribute("aria-label", `Hauteur de ${device.name}`);
    item.querySelector(".device-name-text").textContent = device.name;
    item.querySelector(".device-name").setAttribute("aria-label", `Réglages de ${device.name}`);
    item.querySelector(".device-buttons").setAttribute("aria-label", device.name);

    if (pane.dataset.preview !== undefined) {
      continue;  // The user is choosing a height: leave the preview alone.
    }
    const target = targets.get(device.id);
    if (target && (Math.abs(target.percent - device.position) <= 2 || Date.now() > target.until)) {
      targets.delete(device.id);
    }
    showTarget(item, targets.has(device.id) ? targets.get(device.id).percent : null, false);
    item.querySelector(".device-position").textContent = String(position);
    pane.setAttribute("aria-valuenow", String(position));
    pane.setAttribute("aria-valuetext", `${position} % ouvert`);
  }
  for (const item of Array.from(elements.devices.querySelectorAll(".device"))) {
    if (!seen.has(item.dataset.id)) {
      item.remove();
    }
  }
  arrangeByRoom();
  renderCommandAvailability();
}

const NO_ROOM_LABEL = "Sans pièce";

function roomOf(device) {
  return (device.room || "").trim();
}

function roomNames() {
  return [...new Set(devices.map(roomOf).filter(Boolean))]
    .sort((a, b) => a.localeCompare(b, "fr", { sensitivity: "base" }));
}

function roomHeader(room) {
  const existing = [...elements.devices.querySelectorAll(".room-header")].find((header) => header.dataset.room === room);
  if (existing) {
    return existing;
  }
  const header = document.createElement("li");
  header.className = "room-header";
  header.dataset.room = room;
  header.textContent = room || NO_ROOM_LABEL;
  return header;
}

// Rooms in alphabetical order, shutters without a room last. Without any
// room the façade stays a plain grid, as before rooms existed.
function arrangeByRoom() {
  const rooms = roomNames();
  const order = devices.some((device) => !roomOf(device)) ? [...rooms, ""] : rooms;
  const wanted = [];
  for (const room of order) {
    if (rooms.length > 0) {
      wanted.push(roomHeader(room));
    }
    devices
      .filter((device) => roomOf(device) === room)
      .sort((a, b) => a.name.localeCompare(b.name, "fr", { sensitivity: "base" }))
      .forEach((device) => wanted.push(findItem(device.id)));
  }
  for (const header of elements.devices.querySelectorAll(".room-header")) {
    if (!wanted.includes(header)) {
      header.remove();
    }
  }
  // Move only what is out of place, so a window being dragged keeps its pointer.
  wanted.forEach((node, index) => {
    if (elements.devices.children[index] !== node) {
      elements.devices.insertBefore(node, elements.devices.children[index] || null);
    }
  });
}

function commitTarget(deviceId, percent) {
  const device = devices.find((entry) => entry.id === deviceId);
  const travelMs = ((device && device.travel_time) || 30) * 1000;
  targets.set(deviceId, { percent, until: Date.now() + travelMs + TARGET_GRACE_MS });
  renderDevices();
  runPosition(deviceId, percent);
}

// The window is the slider: drag (or tap) to choose a height, arrow keys from the keyboard.
function attachWindowControl(pane, deviceId) {
  let dragging = false;
  let keyTimer = 0;

  const preview = (percent) => {
    const item = pane.closest(".device");
    pane.dataset.preview = String(percent);
    showTarget(item, percent, true);
    item.querySelector(".device-position").textContent = String(percent);
    pane.setAttribute("aria-valuenow", String(percent));
    pane.setAttribute("aria-valuetext", `${percent} % ouvert`);
  };
  const fromPointer = (event) => {
    const rect = pane.getBoundingClientRect();
    return snap(100 - ((event.clientY - rect.top) / rect.height) * 100);
  };
  const release = (send) => {
    if (!dragging) {
      return;
    }
    dragging = false;
    const percent = Number(pane.dataset.preview);
    delete pane.dataset.preview;
    if (send) {
      commitTarget(deviceId, percent);
    } else {
      renderDevices();
    }
  };

  pane.addEventListener("pointerdown", (event) => {
    if (!commandsEnabled() || event.button > 0) {
      return;
    }
    dragging = true;
    try {
      pane.setPointerCapture(event.pointerId);
    } catch {
      // Synthetic or already released pointer: the drag still works without capture.
    }
    preview(fromPointer(event));
    event.preventDefault();
  });
  pane.addEventListener("pointermove", (event) => {
    if (dragging) {
      preview(fromPointer(event));
    }
  });
  pane.addEventListener("pointerup", () => release(true));
  pane.addEventListener("pointercancel", () => release(false));

  pane.addEventListener("keydown", (event) => {
    if (!commandsEnabled()) {
      return;
    }
    const steps = { ArrowUp: STEP, ArrowRight: STEP, ArrowDown: -STEP, ArrowLeft: -STEP, PageUp: 25, PageDown: -25 };
    const base = pane.dataset.preview !== undefined
      ? Number(pane.dataset.preview)
      : Number(pane.getAttribute("aria-valuenow") || 0);
    let percent;
    if (event.key in steps) {
      percent = snap(base + steps[event.key]);
    } else if (event.key === "Home") {
      percent = 0;
    } else if (event.key === "End") {
      percent = 100;
    } else {
      return;
    }
    event.preventDefault();
    preview(percent);
    clearTimeout(keyTimer);
    keyTimer = setTimeout(() => {
      delete pane.dataset.preview;
      commitTarget(deviceId, percent);
    }, KEY_SEND_DELAY_MS);
  });
}

function createDeviceItem(deviceId) {
  const item = document.createElement("li");
  item.className = "device";
  item.dataset.id = deviceId;

  const pane = document.createElement("div");
  pane.className = "window";
  pane.tabIndex = 0;
  pane.dataset.command = "true";
  pane.setAttribute("role", "slider");
  pane.setAttribute("aria-orientation", "vertical");
  pane.setAttribute("aria-valuemin", "0");
  pane.setAttribute("aria-valuemax", "100");
  const glass = document.createElement("div");
  glass.className = "glass";
  const shutter = document.createElement("div");
  shutter.className = "shutter";
  const targetLine = document.createElement("div");
  targetLine.className = "target-line";
  pane.append(glass, shutter, targetLine);
  attachWindowControl(pane, deviceId);

  const name = document.createElement("button");
  name.type = "button";
  name.className = "device-name";
  const nameText = document.createElement("span");
  nameText.className = "device-name-text";
  name.append(nameText, icon("edit"));
  name.addEventListener("click", () => openDeviceDialog(deviceId));

  const reading = document.createElement("p");
  reading.className = "device-reading";
  const value = document.createElement("span");
  value.className = "device-position";
  const unit = document.createElement("span");
  unit.className = "device-unit";
  unit.textContent = "% ouvert";
  reading.append(value, unit);

  const buttons = document.createElement("div");
  buttons.className = "device-buttons";
  buttons.setAttribute("role", "group");
  buttons.append(
    button(icon("open"), "Ouvrir", () => runButton(deviceId, "open")),
    button(icon("stop"), "Stop", () => runButton(deviceId, "stop")),
    button(icon("close"), "Fermer", () => runButton(deviceId, "close"))
  );

  const status = document.createElement("p");
  const known = deviceStatus.get(deviceId);
  status.className = `device-status ${known ? known.kind : ""}`;
  status.textContent = known ? known.text : "";
  status.setAttribute("role", "status");

  item.append(pane, name, reading, buttons, status);
  return item;
}

function renderCommandAvailability() {
  const enabled = commandsEnabled();
  for (const element of elements.devices.querySelectorAll("[data-command]")) {
    if (element instanceof HTMLButtonElement) {
      element.disabled = !enabled;
    } else {
      element.setAttribute("aria-disabled", String(!enabled));
    }
  }
}

function renderConnection() {
  const status = elements.connection;
  if (!brokerConnected) {
    status.textContent = "Pas de connexion";
    status.className = "status status-off";
  } else if (!boardOnline) {
    status.textContent = "Boîtier hors ligne";
    status.className = "status status-offline";
  } else {
    status.textContent = "Boîtier en ligne";
    status.className = "status status-online";
  }
  renderCommandAvailability();
}

function handleState(payload) {
  const list = payload && payload.values && payload.values.devices;
  if (Array.isArray(list)) {
    devices = list;
    renderDevices();
  }
}

function disconnect() {
  if (client) {
    client.removeAllListeners();
    client.on("error", () => {});
    client.end(true);
    client = null;
  }
  brokerConnected = false;
  boardOnline = false;
  renderConnection();
}

function connect() {
  disconnect();
  client = mqtt.connect(BROKER_URL, {
    username: config.token,
    clientId: `webapp-${crypto.randomUUID()}`,
    clean: true,
    keepalive: 30,
    connectTimeout: 8000,
    reconnectPeriod: 2000,
    // The default uses a blob worker, which the page's Content-Security-Policy forbids.
    timerVariant: "native"
  });

  client.on("connect", () => {
    brokerConnected = true;
    boardOnline = false;
    renderConnection();
    client.subscribe([TOPIC_STATE, TOPIC_STATUS, TOPIC_ACK], { qos: 1 });
  });
  client.on("close", () => {
    brokerConnected = false;
    renderConnection();
  });
  client.on("error", (error) => {
    // CONNACK 4 or 5: invalid token or disallowed origin. Retrying cannot help.
    if (error && (error.code === 4 || error.code === 5)) {
      disconnect();
      showSettings("Jeton flespi refusé.");
    }
  });
  client.on("message", (topic, message) => {
    const text = message.toString();
    if (topic === TOPIC_STATUS) {
      boardOnline = text === "online";
      renderConnection();
      return;
    }
    let payload;
    try {
      payload = JSON.parse(text);
    } catch {
      return;
    }
    if (topic === TOPIC_STATE) {
      handleState(payload);
    } else if (topic === TOPIC_ACK) {
      handleAck(payload);
    }
  });
}

function showDevices() {
  elements["settings-view"].hidden = true;
  elements["devices-view"].hidden = false;
}

function showSettings(error) {
  elements["devices-view"].hidden = true;
  elements["settings-view"].hidden = false;
  elements.token.value = config ? config.token : "";
  elements["command-key"].value = config ? config.commandKey : "";
  elements["close-settings"].hidden = !config || Boolean(error);
  elements.logout.hidden = !config;
  elements["settings-error"].textContent = error || "";
  elements["settings-error"].hidden = !error;
}

async function start() {
  signingKey = await importSigningKey(config.commandKey);
  showDevices();
  connect();
}

function openDeviceDialog(deviceId) {
  const device = devices.find((entry) => entry.id === deviceId);
  if (!device) {
    return;
  }
  editedDeviceId = deviceId;
  elements["device-name"].value = device.name;
  elements["device-room"].value = roomOf(device);
  elements["room-list"].replaceChildren(...roomNames().map((room) => {
    const option = document.createElement("option");
    option.value = room;
    return option;
  }));
  elements["device-time"].value = device.travel_time;
  elements["device-repeat"].checked = Boolean(device.repeat);
  elements["device-error"].hidden = true;
  elements["device-dialog"].showModal();
}

elements["settings-form"].addEventListener("submit", async (event) => {
  event.preventDefault();
  const token = elements.token.value.trim();
  const commandKey = elements["command-key"].value.trim();
  if (!token || !commandKey) {
    showError("Les deux champs sont obligatoires.");
    return;
  }
  config = { token, commandKey };
  saveConfig(config);
  await start();
});

function showError(text) {
  elements["settings-error"].textContent = text;
  elements["settings-error"].hidden = false;
}

elements["open-settings"].addEventListener("click", () => showSettings());
elements["close-settings"].addEventListener("click", showDevices);
elements.logout.addEventListener("click", () => {
  disconnect();
  clearConfig();
  config = null;
  signingKey = null;
  devices = [];
  deviceStatus.clear();
  targets.clear();
  elements.devices.replaceChildren();
  showSettings();
});

elements["device-cancel"].addEventListener("click", () => elements["device-dialog"].close());
elements["device-form"].addEventListener("submit", async (event) => {
  event.preventDefault();
  const device = devices.find((entry) => entry.id === editedDeviceId);
  if (!device) {
    elements["device-dialog"].close();
    return;
  }
  const deviceId = device.id;
  const name = elements["device-name"].value.trim();
  const seconds = Number(elements["device-time"].value);
  const repeat = elements["device-repeat"].checked;
  const room = elements["device-room"].value.trim();
  if (!name || !Number.isInteger(seconds) || seconds < 1 || seconds > 600) {
    elements["device-error"].textContent = "Nom requis, temps de course entre 1 et 600 secondes.";
    elements["device-error"].hidden = false;
    return;
  }
  const commands = [];
  if (name !== device.name) {
    commands.push(["edit1W", { deviceId, name }]);
  }
  if (seconds !== device.travel_time) {
    commands.push(["time1W", { deviceId, seconds }]);
  }
  if (repeat !== Boolean(device.repeat)) {
    commands.push(["repeat1W", { deviceId, enabled: repeat }]);
  }
  if (room !== roomOf(device)) {
    commands.push(["room1W", { deviceId, room }]);
  }
  elements["device-dialog"].close();
  if (commands.length === 0) {
    return;
  }
  setDeviceStatus(deviceId, "En cours…", "pending");
  for (const [action, params] of commands) {
    const error = await sendCommand(action, params);
    if (error) {
      setDeviceStatus(deviceId, error, "error");
      return;
    }
  }
  setDeviceStatus(deviceId, "Réglages enregistrés", "ok");
});

// iOS cuts WebSockets in the background, sometimes without any close event:
// start over on return and read the retained state again.
document.addEventListener("visibilitychange", () => {
  if (document.hidden) {
    hiddenSince = Date.now();
  } else if (config && signingKey && (!brokerConnected || Date.now() - hiddenSince > RECONNECT_AFTER_HIDDEN_MS)) {
    connect();
  }
});
window.addEventListener("online", () => {
  if (config && signingKey) {
    connect();
  }
});

if ("serviceWorker" in navigator) {
  navigator.serviceWorker.register("sw.js").catch(() => {});
}

if (config) {
  start();
} else {
  showSettings();
}
