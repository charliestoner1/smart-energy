const BROKER_URL = 'wss://1e1a4e5c581e4bc3a697f8937d7fb9e4.s1.eu.hivemq.cloud:8884/mqtt';
const PRICING_API_URL = 'http://localhost:5000';

const mqttOptions = {
  username: 'omeravi',
  password: 'Omeromer1!',
  keepalive: 60,
  reconnectPeriod: 2000,
  connectTimeout: 10000
};

const TOPICS = {
  telemetry: 'sensors/room1/telemetry',
  control: 'control/room1/cmd',
  alerts: 'alerts/room1/anomaly',
  stateFan: 'control/room1/state/fan',
  stateLamp: 'control/room1/state/lamp'
};

const RATES = [
  { block: 250, rate: 0.110 },
  { block: 500, rate: 0.145 },
  { block: Infinity, rate: 0.185 }
];

const TIER_INFO = {
  0: { name: 'VERY LOW', color: '#00d9ff', class: 'tier-very_low' },
  1: { name: 'LOW', color: '#51cf66', class: 'tier-low' },
  2: { name: 'NORMAL', color: '#ffd43b', class: 'tier-normal' },
  3: { name: 'HIGH', color: '#ff922b', class: 'tier-high' },
  4: { name: 'VERY HIGH', color: '#ff6b6b', class: 'tier-very_high' },
  5: { name: 'CRITICAL', color: '#e94560', class: 'tier-critical' }
};

let mqttClient = null;
let overrideActive = false;
let accumulatedWh = 0;
let lastSampleTsMs = null;
let currentPricingData = null;
let currentEcoMode = true;
let pricingUpdateInterval = null;

const deviceStates = { fan: false, lamp: false, pir: false };
let roomData = {
  A: { temp: 0, humidity: 0, occupied: false, distance: -1 },
  B: { temp: 0, humidity: 0, occupied: false, distance: -1 }
};

const powerData = { labels: [], datasets: [{ label: 'Power (W)', data: [], borderColor: '#00d9ff', tension: 0.35, pointRadius: 0 }] };
const tempData = {
  labels: [],
  datasets: [
    { label: 'Room A (°C)', data: [], borderColor: '#e94560', tension: 0.35, pointRadius: 0, yAxisID: 'y' },
    { label: 'Room B (°C)', data: [], borderColor: '#ff922b', tension: 0.35, pointRadius: 0, yAxisID: 'y' },
    { label: 'Humidity A (%)', data: [], borderColor: '#00d9ff', tension: 0.35, pointRadius: 0, yAxisID: 'y1' },
    { label: 'Humidity B (%)', data: [], borderColor: '#3b82f6', tension: 0.35, pointRadius: 0, yAxisID: 'y1' }
  ]
};
const pricingData = {
  labels: [],
  datasets: [{ label: 'Price (¢/kWh)', data: [], borderColor: '#ffd43b', backgroundColor: 'rgba(255, 212, 59, 0.1)', tension: 0.35, pointRadius: 2, fill: true }]
};

let powerChart, tempChart, pricingChart;

function initCharts() {
  const opts = { responsive: true, maintainAspectRatio: true, animation: false };
  
  powerChart = new Chart(document.getElementById('powerChart'), {
    type: 'line', data: powerData,
    options: { ...opts, scales: { y: { beginAtZero: true, title: { display: true, text: 'Power (W)' } } } }
  });
  
  tempChart = new Chart(document.getElementById('tempChart'), {
    type: 'line', data: tempData,
    options: { ...opts, scales: {
      y: { position: 'left', title: { display: true, text: 'Temp (°C)' } },
      y1: { position: 'right', title: { display: true, text: 'Humidity (%)' }, grid: { drawOnChartArea: false } }
    }}
  });
  
  pricingChart = new Chart(document.getElementById('pricingChart'), {
    type: 'line', data: pricingData,
    options: { ...opts, scales: {
      y: { beginAtZero: true, title: { display: true, text: 'Price (¢/kWh)' }, ticks: { callback: v => v.toFixed(2) + '¢' } },
      x: { title: { display: true, text: 'Time' } }
    }}
  });
}

function addDataPoint(chart, data, label, value) {
  data.labels.push(label);
  if (Array.isArray(value)) value.forEach((v, i) => data.datasets[i].data.push(v));
  else data.datasets[0].data.push(value);
  if (data.labels.length > 60) { data.labels.shift(); data.datasets.forEach(ds => ds.data.shift()); }
  chart.update('none');
}

function connectMQTT() {
  mqttClient = mqtt.connect(BROKER_URL, mqttOptions);
  
  mqttClient.on('connect', () => {
    setBrokerStatus(true);
    fetchPricingData();
    fetchPriceHistory();
    pricingUpdateInterval = setInterval(fetchPricingData, 300000);
    mqttClient.subscribe([TOPICS.telemetry, TOPICS.alerts, TOPICS.stateFan, TOPICS.stateLamp]);
  });
  
  mqttClient.on('reconnect', () => setBrokerStatus(false));
  mqttClient.on('close', () => setBrokerStatus(false));
  mqttClient.on('error', () => setBrokerStatus(false));
  
  mqttClient.on('message', (topic, payload) => {
    let data;
    try { data = JSON.parse(payload.toString()); } catch { return; }
    if (topic === TOPICS.telemetry) handleTelemetry(data);
    else if (topic === TOPICS.alerts) handleAlert(data);
    else if (topic === TOPICS.stateFan) handleStateEcho('fan', data);
    else if (topic === TOPICS.stateLamp) handleStateEcho('lamp', data);
  });
}

// ============================================
// Handle MQTT Messages
// ============================================
function handleMQTTMessage(topic, data) {
  if (topic === CONFIG.TOPICS.TELEMETRY) {
    // Expected format: {"ts": 1738176000, "tC": 24.1, "rh": 58, "pir": 1, "amps": 0.42}
    updateTelemetry(data)
  } else if (topic === CONFIG.TOPICS.CONTROL_ACK) {
    console.log("[v0] Control acknowledged:", data)
  } else if (topic === CONFIG.TOPICS.ALERTS) {
    addAlert(data.message || JSON.stringify(data))
  }
}

function handleTelemetry(d) {
  const timeLabel = new Date().toLocaleTimeString();
  const tempA = d.tC_A ?? d.tC ?? 0, tempB = d.tC_B ?? d.tC ?? 0;
  const humidA = d.rh_A ?? d.rh ?? 0, humidB = d.rh_B ?? d.rh ?? 0;
  const occA = d.occ_A ?? (d.pir === 1), occB = d.occ_B ?? false;
  
  roomData.A = { temp: tempA, humidity: humidA, occupied: occA, distance: d.dist_A ?? -1 };
  roomData.B = { temp: tempB, humidity: humidB, occupied: occB, distance: d.dist_B ?? -1 };
  currentEcoMode = d.eco_mode === 1;
  updateModeDisplay();
  if (d.price_cents !== undefined) updatePricingFromTelemetry(d);
  
  const voltage = d.voltage ?? 120, amps = d.amps ?? 0;
  const powerW = +(voltage * amps).toFixed(1);
  addDataPoint(powerChart, powerData, timeLabel, powerW);
  addDataPoint(tempChart, tempData, timeLabel, [tempA, tempB, humidA, humidB]);
  
  deviceStates.pir = occA || occB;
  updateOccupancyStatus();
  
  const tsMs = d.ts ?? Date.now();
  if (lastSampleTsMs != null && tsMs > lastSampleTsMs) accumulatedWh += powerW * ((tsMs - lastSampleTsMs) / 3600000);
  lastSampleTsMs = tsMs;
  updateProjectedCost();
  updateRoomCards();
}

function updateModeDisplay() {
  const el = document.getElementById('modeStatus');
  if (el) {
    el.textContent = currentEcoMode ? 'ECO MODE' : 'MANUAL MODE';
    el.className = currentEcoMode ? 'mode-badge eco' : 'mode-badge manual';
  }
}

function updateOccupancyStatus() {
  const el = document.getElementById('statusPIR');
  const occA = roomData.A.occupied, occB = roomData.B.occupied;
  let status = 'Rooms: ';
  if (occA && occB) status += 'A & B Occupied';
  else if (occA) status += 'A Occupied';
  else if (occB) status += 'B Occupied';
  else status += 'All Empty';
  el.textContent = status;
  el.classList.toggle('active', occA || occB);
}

function updateRoomCards() {
  const roomAEl = document.getElementById('roomAInfo');
  const roomBEl = document.getElementById('roomBInfo');
  if (roomAEl) roomAEl.innerHTML = `<strong>Room A</strong><br>Temp: ${roomData.A.temp.toFixed(1)}°C<br>Humidity: ${roomData.A.humidity.toFixed(0)}%<br>Status: ${roomData.A.occupied ? '🟢 Occupied' : '⚪ Empty'}`;
  if (roomBEl) roomBEl.innerHTML = `<strong>Room B</strong><br>Temp: ${roomData.B.temp.toFixed(1)}°C<br>Humidity: ${roomData.B.humidity.toFixed(0)}%<br>Status: ${roomData.B.occupied ? '🟢 Occupied' : '⚪ Empty'}`;
}

function updatePricingFromTelemetry(d) {
  const tier = d.price_tier ?? 2;
  const tierInfo = TIER_INFO[tier];
  const pricingDiv = document.getElementById('pricingInfo');
  if (pricingDiv) {
    pricingDiv.innerHTML = `<strong>Price:</strong> ${(d.price_cents ?? 0).toFixed(2)}¢/kWh<br><strong>Tier:</strong> <span style="color: ${tierInfo.color}">${tierInfo.name}</span><br><strong>Action:</strong> ${d.price_action ?? 'normal'}<br><strong>Mode:</strong> ${currentEcoMode ? '🌿 ECO' : '🔧 MANUAL'}`;
    pricingDiv.className = 'pricing-info ' + tierInfo.class;
  }
}

function handleAlert(d) {
  const div = document.getElementById('alerts');
  div.innerHTML = `<div class="alert-item">⚠️ ${d.message || 'Anomaly detected'}</div>` + div.innerHTML;
}

function publishCmd(device, action, reason = 'manual') {
  if (!mqttClient || !mqttClient.connected) { alert('Broker not connected'); return; }
  mqttClient.publish(TOPICS.control, JSON.stringify({ device, action, reason }));
}

// ============================================
// Send Control Command
// ============================================
function sendCommand(device, action) {
  if (overrideActive) { alert('Override active'); return; }
  if (currentEcoMode) { alert('System in ECO mode - devices controlled automatically'); return; }
  publishCmd(device, action, 'dashboard-manual');
  deviceStates[device] = action === 'on';
  updateDeviceStatus(device);
}

function toggleOverride() {
  overrideActive = !overrideActive;
  const btn = document.getElementById('overrideBtn');
  const banner = document.getElementById('overrideBanner');
  if (overrideActive) {
    publishCmd('fan', 'off', 'override');
    publishCmd('lamp', 'off', 'override');
    deviceStates.fan = false; deviceStates.lamp = false;
    updateDeviceStatus('fan'); updateDeviceStatus('lamp');
    btn.textContent = '✓ OVERRIDE ACTIVE — CLICK TO RESTORE';
    btn.classList.add('active');
    banner.style.display = 'block';
  } else {
    btn.textContent = '🚨 EMERGENCY OVERRIDE';
    btn.classList.remove('active');
    banner.style.display = 'none';
  }

  mqttClient.publish(CONFIG.TOPICS.CONTROL_CMD, JSON.stringify(command), { qos: 1 })

function updateDeviceStatus(device) {
  const el = document.getElementById(`status${device.charAt(0).toUpperCase()}${device.slice(1)}`);
  const on = !!deviceStates[device];
  el.textContent = `${device.charAt(0).toUpperCase()}${device.slice(1)}: ${on ? 'ON' : 'OFF'}`;
  el.classList.toggle('active', on);
}

function updateProjectedCost() {
  const kWh = accumulatedWh / 1000;
  let cost;
  if (currentPricingData && currentPricingData.price_cents_per_kwh) {
    cost = kWh * (currentPricingData.price_cents_per_kwh / 100);
  } else {
    let remaining = kWh; cost = 0;
    for (const tier of RATES) { const block = Math.min(remaining, tier.block); cost += block * tier.rate; remaining -= block; if (remaining <= 0) break; }
  }
  document.getElementById('projectedCost').textContent = cost.toFixed(2);
}

async function fetchPricingData() {
  try {
    const response = await fetch(`${PRICING_API_URL}/api/price/current`);
    if (response.ok) {
      currentPricingData = await response.json();
      updatePricingDisplay();
      updateProjectedCost();
    }
  } catch (error) { console.error('Pricing fetch error:', error); }
}

function updatePricingDisplay() {
  if (!currentPricingData) return;
  const price = currentPricingData.price_cents_per_kwh;
  const tier = currentPricingData.tier;
  const rec = currentPricingData.recommendation;
  const tierNum = { 'very_low': 0, 'low': 1, 'normal': 2, 'high': 3, 'very_high': 4, 'critical': 5 }[tier] ?? 2;
  const tierInfo = TIER_INFO[tierNum];
  
  const pricingDiv = document.getElementById('pricingInfo');
  if (pricingDiv) {
    pricingDiv.innerHTML = `<strong>Price:</strong> ${price.toFixed(2)}¢/kWh<br><strong>Tier:</strong> <span style="color: ${tierInfo.color}">${tierInfo.name}</span><br><strong>Action:</strong> ${rec.action}<br><strong>Suggestion:</strong> ${rec.message}<br><strong>Mode:</strong> ${currentEcoMode ? '🌿 ECO' : '🔧 MANUAL'}`;
    pricingDiv.className = 'pricing-info ' + tierInfo.class;
  }
  addDataPoint(pricingChart, pricingData, new Date(currentPricingData.timestamp).toLocaleTimeString(), price);
}

async function fetchPriceHistory() {
  try {
    const response = await fetch(`${PRICING_API_URL}/api/price/history?hours=6`);
    if (response.ok) {
      const data = await response.json();
      pricingData.labels = []; pricingData.datasets[0].data = [];
      data.data.reverse().forEach(r => {
        pricingData.labels.push(new Date(r.millisUTC).toLocaleTimeString());
        pricingData.datasets[0].data.push(r.price_cents_per_kwh);
      });
      pricingChart.update('none');
    }
  } catch (error) { console.error('Price history error:', error); }
}

document.addEventListener('DOMContentLoaded', () => { initCharts(); connectMQTT(); });

window.sendCommand = sendCommand;
window.toggleOverride = toggleOverride;
window.mqttClient = mqttClient;
window.fetchPricingData = fetchPricingData;
window.fetchPriceHistory = fetchPriceHistory;
