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
  "Invalid seconds": "Temps de course invalide"
};

const elements = Object.fromEntries(
  ["connection", "open-settings", "devices-view", "devices", "empty", "settings-view", "settings-error",
    "settings-form", "token", "command-key", "close-settings", "logout",
    "device-dialog", "device-form", "device-name", "device-time", "device-repeat", "device-error",
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

function setDeviceStatus(deviceId, text, kind) {
  deviceStatus.set(deviceId, { text, kind });
  const line = elements.devices.querySelector(`[data-id="${CSS.escape(deviceId)}"] .device-status`);
  if (line) {
    line.textContent = text;
    line.className = `device-status ${kind}`;
  }
}

function button(label, ariaLabel, onClick) {
  const element = document.createElement("button");
  element.type = "button";
  element.textContent = label;
  element.setAttribute("aria-label", ariaLabel);
  element.dataset.command = "true";
  element.addEventListener("click", onClick);
  return element;
}

function renderDevices() {
  elements.empty.hidden = devices.length > 0;
  const seen = new Set();
  for (const device of devices) {
    seen.add(device.id);
    let item = elements.devices.querySelector(`[data-id="${CSS.escape(device.id)}"]`);
    if (!item) {
      item = createDeviceItem(device.id);
      elements.devices.appendChild(item);
    }
    item.style.setProperty("--position", `${device.position}%`);
    item.querySelector(".device-name").textContent = device.name;
    item.querySelector(".device-position").textContent = `${device.position} %`;
  }
  for (const item of Array.from(elements.devices.children)) {
    if (!seen.has(item.dataset.id)) {
      item.remove();
    }
  }
  renderCommandAvailability();
}

function createDeviceItem(deviceId) {
  const item = document.createElement("li");
  item.className = "device";
  item.dataset.id = deviceId;

  const head = document.createElement("div");
  head.className = "device-head";
  const name = document.createElement("span");
  name.className = "device-name";
  const position = document.createElement("span");
  position.className = "device-position";
  const edit = button("✎", "Modifier le volet", () => openDeviceDialog(deviceId));
  edit.classList.add("device-edit");
  head.append(name, position, edit);

  const buttons = document.createElement("div");
  buttons.className = "device-buttons";
  buttons.append(
    button("▲", "Ouvrir", () => runButton(deviceId, "open")),
    button("■", "Stop", () => runButton(deviceId, "stop")),
    button("▼", "Fermer", () => runButton(deviceId, "close"))
  );

  const status = document.createElement("p");
  const known = deviceStatus.get(deviceId);
  status.className = `device-status ${known ? known.kind : ""}`;
  status.textContent = known ? known.text : "";
  status.setAttribute("role", "status");

  item.append(head, buttons, status);
  return item;
}

function renderCommandAvailability() {
  const enabled = brokerConnected && boardOnline;
  for (const element of elements.devices.querySelectorAll("[data-command]")) {
    element.disabled = !enabled;
  }
}

function renderConnection() {
  const chip = elements.connection;
  if (!brokerConnected) {
    chip.textContent = "Broker déconnecté";
    chip.className = "chip chip-off";
  } else if (!boardOnline) {
    chip.textContent = "ESP32 hors ligne";
    chip.className = "chip chip-offline";
  } else {
    chip.textContent = "ESP32 en ligne";
    chip.className = "chip chip-online";
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
