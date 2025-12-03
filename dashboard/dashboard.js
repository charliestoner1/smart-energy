// Configuration
const BROKER_URL = 'wss://1e1a4e5c581e4bc3a697f8937d7fb9e4.s1.eu.hivemq.cloud:8884/mqtt';
const PRICING_API_URL = 'https://smart-energy-production-a08d.up.railway.app';

const mqttOptions = {
  username: 'omeravi',
  password: 'Omeromer1!',
  keepalive: 60,
  reconnectPeriod: 2000,
  connectTimeout: 10000
};

const TOPICS = {
  // Room A (room1)
  telemetryA: 'sensors/room1/telemetry',
  controlA: 'control/room1/cmd',
  alertsA: 'alerts/room1/anomaly',
  // Room B (room2)
  telemetryB: 'sensors/room2/telemetry',
  controlB: 'control/room2/cmd',
  alertsB: 'alerts/room2/anomaly',
  // Shared
  mode: 'control/mode'  // Global mode for all rooms
};

// State
let mqttClient = null;
let overrideActive = false;
let accumulatedWh = 0;
let lastSampleTsMs = null;
let currentPricingData = null;
let currentMode = 'eco';  // 'eco' or 'manual'

const deviceStates = {
  fanA: false, lampA: false, pirA: false,
  fanB: false, lampB: false, pirB: false
};

// Chart configuration
Chart.defaults.color = '#8a8a9a';
Chart.defaults.borderColor = '#2a2a3a';

const chartOptions = {
  responsive: true,
  maintainAspectRatio: true,
  animation: false,
  plugins: { legend: { display: false } },
  scales: {
    x: { grid: { display: false } },
    y: { grid: { color: '#2a2a3a' } }
  }
};

// Initialize charts
const powerChart = new Chart(document.getElementById('powerChart'), {
  type: 'line',
  data: {
    labels: [],
    datasets: [{
      label: 'Power (W)',
      data: [],
      borderColor: '#667eea',
      backgroundColor: '#667eea20',
      tension: 0.4,
      fill: true,
      pointRadius: 0
    }]
  },
  options: {
    ...chartOptions,
    scales: {
      ...chartOptions.scales,
      y: { ...chartOptions.scales.y, beginAtZero: true }
    }
  }
});

const tempChart = new Chart(document.getElementById('tempChart'), {
  type: 'line',
  data: {
    labels: [],
    datasets: [
      {
        label: 'Room A',
        data: [],
        borderColor: '#667eea',
        tension: 0.4,
        pointRadius: 0
      },
      {
        label: 'Room B',
        data: [],
        borderColor: '#f093fb',
        tension: 0.4,
        pointRadius: 0
      }
    ]
  },
  options: {
    ...chartOptions,
    plugins: {
      legend: { display: true, position: 'top', labels: { boxWidth: 12 } }
    }
  }
});

const pricingChart = new Chart(document.getElementById('pricingChart'), {
  type: 'line',
  data: {
    labels: [],
    datasets: [{
      label: 'Price (¢/kWh)',
      data: [],
      borderColor: '#764ba2',
      backgroundColor: '#764ba220',
      tension: 0.4,
      fill: true,
      pointRadius: 2
    }]
  },
  options: {
    ...chartOptions,
    plugins: {
      legend: { display: false },
      tooltip: {
        callbacks: {
          label: ctx => `${ctx.parsed.y.toFixed(2)}¢/kWh`
        }
      }
    },
    scales: {
      x: { grid: { display: false } },
      y: {
        grid: { color: '#2a2a3a' },
        ticks: { callback: v => v + '¢' }
      }
    }
  }
});

// ===== MODE SWITCHING =====

function setMode(mode) {
  if (overrideActive) {
    addAlert('warning', 'Deactivate emergency override first to change modes.');
    return;
  }
  
  currentMode = mode;
  
  // Update UI
  const ecoBtn = document.getElementById('ecoModeBtn');
  const manualBtn = document.getElementById('manualModeBtn');
  const modeInfoBanner = document.getElementById('modeInfoBanner');
  const modeInfoText = document.getElementById('modeInfoText');
  const modeInfoIcon = modeInfoBanner.querySelector('.mode-info-icon');
  
  // Update toggle buttons
  ecoBtn.classList.toggle('active', mode === 'eco');
  manualBtn.classList.toggle('active', mode === 'manual');
  
  // Update info banner
  if (mode === 'eco') {
    modeInfoBanner.className = 'mode-info-banner eco';
    modeInfoIcon.textContent = '🌿';
    modeInfoText.textContent = 'Eco Mode: Automatic control based on electricity pricing and occupancy';
  } else {
    modeInfoBanner.className = 'mode-info-banner manual';
    modeInfoIcon.textContent = '🔧';
    modeInfoText.textContent = 'Manual Mode: You have full control over all devices';
  }
  
  // Update room card overlays and control states
  updateControlsState();
  
  // Publish mode change via MQTT
  publishModeChange(mode);
  
  // Add alert
  const modeIcon = mode === 'eco' ? '🌿' : '🔧';
  addAlert('info', `${modeIcon} Switched to ${mode.charAt(0).toUpperCase() + mode.slice(1)} Mode`);
  
  console.log(`Mode changed to: ${mode}`);
}

function updateControlsState() {
  const isManual = currentMode === 'manual' && !overrideActive;
  
  // Get all control buttons
  const controlButtons = [
    'fanAOnBtn', 'fanAOffBtn', 'lampAOnBtn', 'lampAOffBtn',
    'fanBOnBtn', 'fanBOffBtn', 'lampBOnBtn', 'lampBOffBtn'
  ];
  
  // Enable/disable buttons based on mode
  controlButtons.forEach(id => {
    const btn = document.getElementById(id);
    if (btn) {
      btn.disabled = !isManual;
      btn.classList.toggle('disabled', !isManual);
    }
  });
  
  // Show/hide eco overlays on room cards
  const ecoOverlayA = document.getElementById('ecoOverlayA');
  const ecoOverlayB = document.getElementById('ecoOverlayB');
  
  if (ecoOverlayA) ecoOverlayA.classList.toggle('active', currentMode === 'eco' && !overrideActive);
  if (ecoOverlayB) ecoOverlayB.classList.toggle('active', currentMode === 'eco' && !overrideActive);
  
  // Update room controls container opacity
  const controlsA = document.getElementById('controlsRoomA');
  const controlsB = document.getElementById('controlsRoomB');
  
  if (controlsA) controlsA.classList.toggle('eco-disabled', currentMode === 'eco' && !overrideActive);
  if (controlsB) controlsB.classList.toggle('eco-disabled', currentMode === 'eco' && !overrideActive);
}

function publishModeChange(mode) {
  if (!mqttClient?.connected) return;
  
  const payload = {
    mode: mode,
    ecoMode: mode === 'eco' ? 1 : 0,
    timestamp: Date.now()
  };
  
  mqttClient.publish(TOPICS.mode, JSON.stringify(payload), { retain: true });
  console.log('Published mode change:', payload);
}

function updateModeDisplay() {
  // This function is called when telemetry contains mode info from ESP32
  // The ESP32 can also change modes via physical buttons
  const ecoBtn = document.getElementById('ecoModeBtn');
  const manualBtn = document.getElementById('manualModeBtn');
  
  ecoBtn.classList.toggle('active', currentMode === 'eco');
  manualBtn.classList.toggle('active', currentMode === 'manual');
  
  updateControlsState();
}

// ===== MQTT CONNECTION =====

function connectMQTT() {
  mqttClient = mqtt.connect(BROKER_URL, mqttOptions);

  mqttClient.on('connect', () => {
    updateConnectionStatus(true);
    // Subscribe to both rooms
    mqttClient.subscribe([
      TOPICS.telemetryA, TOPICS.alertsA,
      TOPICS.telemetryB, TOPICS.alertsB,
      TOPICS.mode
    ]);
    addAlert('info', 'Connected to MQTT broker');
    
    // Publish initial mode
    publishModeChange(currentMode);
  });

  mqttClient.on('reconnect', () => updateConnectionStatus(false));
  mqttClient.on('close', () => updateConnectionStatus(false));
  mqttClient.on('error', err => {
    console.error('MQTT error:', err);
    updateConnectionStatus(false);
  });

  mqttClient.on('message', (topic, payload) => {
    try {
      const data = JSON.parse(payload.toString());
      if (topic === TOPICS.telemetryA) handleTelemetry(data, 'A');
      else if (topic === TOPICS.telemetryB) handleTelemetry(data, 'B');
      else if (topic === TOPICS.alertsA || topic === TOPICS.alertsB) handleAlert(data);
      else if (topic === TOPICS.mode) handleModeMessage(data);
    } catch (e) { /* ignore invalid JSON */ }
  });
}

function handleModeMessage(data) {
  // Handle mode changes from other clients or ESP32
  if (data.mode && data.mode !== currentMode) {
    currentMode = data.mode;
    updateModeDisplay();
    
    const modeInfoBanner = document.getElementById('modeInfoBanner');
    const modeInfoText = document.getElementById('modeInfoText');
    const modeInfoIcon = modeInfoBanner.querySelector('.mode-info-icon');
    
    if (currentMode === 'eco') {
      modeInfoBanner.className = 'mode-info-banner eco';
      modeInfoIcon.textContent = '🌿';
      modeInfoText.textContent = 'Eco Mode: Automatic control based on electricity pricing and occupancy';
    } else {
      modeInfoBanner.className = 'mode-info-banner manual';
      modeInfoIcon.textContent = '🔧';
      modeInfoText.textContent = 'Manual Mode: You have full control over all devices';
    }
    
    addAlert('info', `Mode changed to ${currentMode} by another device`);
  }
}

function updateConnectionStatus(connected) {
  const el = document.getElementById('connectionStatus');
  el.className = 'connection-status' + (connected ? ' connected' : '');
  el.querySelector('span:last-child').textContent = connected ? 'Connected' : 'Connecting...';
}

// ===== TELEMETRY HANDLING =====

function handleTelemetry(d, room) {
  const tsMs = d.ts > 1e12 ? d.ts : d.ts * 1000;
  const timeLabel = new Date(tsMs).toLocaleTimeString('en-US', { hour: '2-digit', minute: '2-digit' });

  // Power calculation (aggregate from both rooms)
  const voltage = d.voltage || 120;
  const amps = d.amps || 0;
  const powerW = +(voltage * amps).toFixed(1);
  
  // Only add to power chart once per update cycle (use room A as primary)
  if (room === 'A') {
    addDataPoint(powerChart, timeLabel, powerW);
  }

  // Temperature for this specific room
  const temp = d.tC !== undefined ? d.tC : null;
  const humidity = d.rh !== undefined ? d.rh : null;
  
  if (temp !== null) {
    document.getElementById('temp' + room).textContent = temp.toFixed(1) + '°C';
    // Add to chart - Room A is dataset 0, Room B is dataset 1
    const datasetIndex = room === 'A' ? 0 : 1;
    tempChart.data.datasets[datasetIndex].data.push(temp);
    
    // Only update labels and trim once (from room A)
    if (room === 'A') {
      tempChart.data.labels.push(timeLabel);
      trimChartData(tempChart, 60);
    }
    tempChart.update('none');
  }

  // Humidity for this room
  if (humidity !== null) {
    document.getElementById('humidity' + room).textContent = humidity.toFixed(0) + '%';
  }

  // Occupancy for this room
  const occupied = d.pir === 1 || d.pir === true;
  updateOccupancy(room, occupied);
  deviceStates['pir' + room] = occupied;

  // Mode from ESP32 (if ESP32 changed mode via physical button)
  if (d.ecoMode !== undefined) {
    const newMode = d.ecoMode ? 'eco' : 'manual';
    if (newMode !== currentMode) {
      currentMode = newMode;
      updateModeDisplay();
      
      const modeInfoBanner = document.getElementById('modeInfoBanner');
      const modeInfoText = document.getElementById('modeInfoText');
      const modeInfoIcon = modeInfoBanner.querySelector('.mode-info-icon');
      
      if (currentMode === 'eco') {
        modeInfoBanner.className = 'mode-info-banner eco';
        modeInfoIcon.textContent = '🌿';
        modeInfoText.textContent = 'Eco Mode: Automatic control based on electricity pricing and occupancy';
      } else {
        modeInfoBanner.className = 'mode-info-banner manual';
        modeInfoIcon.textContent = '🔧';
        modeInfoText.textContent = 'Manual Mode: You have full control over all devices';
      }
    }
  }

  // Device states from telemetry for this room
  if (d.fan !== undefined) {
    deviceStates['fan' + room] = d.fan;
    updateDeviceStatus('fan' + room);
  }
  if (d.lamp !== undefined) {
    deviceStates['lamp' + room] = d.lamp;
    updateDeviceStatus('lamp' + room);
  }

  // Cost calculation (only from room A to avoid double counting)
  if (room === 'A') {
    if (lastSampleTsMs != null && tsMs > lastSampleTsMs) {
      const dtHours = (tsMs - lastSampleTsMs) / 3600000;
      accumulatedWh += powerW * dtHours;
    }
    lastSampleTsMs = tsMs;
    updateProjectedCost();
  }
}

function updateOccupancy(room, occupied) {
  const badge = document.getElementById('occupancy' + room);
  const card = document.getElementById('room' + room);
  
  if (!badge || !card) return;
  
  if (occupied) {
    badge.className = 'occupancy-badge active';
    badge.innerHTML = '<span class="occupancy-dot"></span>Occupied';
    card.classList.add('occupied');
  } else {
    badge.className = 'occupancy-badge';
    badge.innerHTML = '<span class="occupancy-dot"></span>Vacant';
    card.classList.remove('occupied');
  }
}

// ===== CHART HELPERS =====

function addDataPoint(chart, label, value) {
  chart.data.labels.push(label);
  chart.data.datasets[0].data.push(value);
  trimChartData(chart, 60);
  chart.update('none');
}

function trimChartData(chart, max) {
  if (chart.data.labels.length > max) {
    chart.data.labels.shift();
    chart.data.datasets.forEach(ds => ds.data.shift());
  }
}

// ===== PRICING =====

async function fetchPricingData() {
  try {
    const [currentRes, statsRes] = await Promise.all([
      fetch(`${PRICING_API_URL}/api/price/current`),
      fetch(`${PRICING_API_URL}/api/price/stats?hours=24`)
    ]);

    if (currentRes.ok) {
      currentPricingData = await currentRes.json();
      updatePricingDisplay();
    }

    if (statsRes.ok) {
      const stats = await statsRes.json();
      document.getElementById('avgPrice').textContent = stats.avg_price.toFixed(1) + '¢';
      document.getElementById('priceRange').textContent = `${stats.min_price.toFixed(1)} - ${stats.max_price.toFixed(1)}¢`;
    }
  } catch (err) {
    console.error('Pricing fetch error:', err);
  }
}

async function fetchPriceHistory() {
  try {
    const res = await fetch(`${PRICING_API_URL}/api/price/history?hours=6`);
    if (!res.ok) return;

    const data = await res.json();
    pricingChart.data.labels = [];
    pricingChart.data.datasets[0].data = [];

    data.data.reverse().forEach(record => {
      const time = new Date(record.millisUTC).toLocaleTimeString('en-US', { hour: '2-digit', minute: '2-digit' });
      pricingChart.data.labels.push(time);
      pricingChart.data.datasets[0].data.push(record.price_cents_per_kwh);
    });

    pricingChart.update('none');
  } catch (err) {
    console.error('Price history error:', err);
  }
}

function updatePricingDisplay() {
  if (!currentPricingData) return;

  const price = currentPricingData.price_cents_per_kwh;
  const tier = currentPricingData.tier;
  const rec = currentPricingData.recommendation;

  document.getElementById('currentPrice').textContent = price.toFixed(1);
  
  const heroEl = document.getElementById('pricingHero');
  heroEl.className = 'pricing-hero tier-' + tier;

  const tierBadge = document.querySelector('.tier-badge');
  tierBadge.className = 'tier-badge ' + tier;
  tierBadge.textContent = tier.replace('_', ' ').toUpperCase();

  document.getElementById('tierMessage').textContent = rec.message;
  document.getElementById('recommendation').textContent = rec.action.toUpperCase();

  // Add to chart
  const timeLabel = new Date().toLocaleTimeString('en-US', { hour: '2-digit', minute: '2-digit' });
  addDataPoint(pricingChart, timeLabel, price);

  updateProjectedCost();
}

function updateProjectedCost() {
  const kWh = accumulatedWh / 1000;
  let cost;
  
  if (currentPricingData) {
    cost = kWh * (currentPricingData.price_cents_per_kwh / 100);
  } else {
    // Fallback GRU rates
    const rates = [
      { block: 250, rate: 0.110 },
      { block: 500, rate: 0.145 },
      { block: Infinity, rate: 0.185 }
    ];
    cost = 0;
    let remaining = kWh;
    for (const tier of rates) {
      const block = Math.min(remaining, tier.block);
      cost += block * tier.rate;
      remaining -= block;
      if (remaining <= 0) break;
    }
  }

  document.getElementById('projectedCost').textContent = cost.toFixed(2);
}

// ===== CONTROLS =====

function sendCommand(device, action) {
  // Check if in Eco mode
  if (currentMode === 'eco' && !overrideActive) {
    addAlert('warning', '🌿 Switch to Manual mode to control devices directly.');
    return;
  }
  
  if (overrideActive) {
    addAlert('warning', 'Manual override is active. Deactivate to control devices.');
    return;
  }

  if (!mqttClient?.connected) {
    addAlert('warning', 'Not connected to broker');
    return;
  }

  // Determine which room's control topic to use
  // device names: fanA, lampA, fanB, lampB
  const room = device.slice(-1); // 'A' or 'B'
  const deviceType = device.slice(0, -1); // 'fan' or 'lamp'
  const controlTopic = room === 'A' ? TOPICS.controlA : TOPICS.controlB;

  const payload = {
    device: deviceType,  // Send just 'fan' or 'lamp' to the room-specific topic
    action,
    reason: 'manual_dashboard',
    mode: currentMode
  };
  
  mqttClient.publish(controlTopic, JSON.stringify(payload));
  
  // Update local state
  deviceStates[device] = action === 'on';
  updateDeviceStatus(device);
  addAlert('info', `Room ${room} ${deviceType} turned ${action.toUpperCase()}`);
}

function updateDeviceStatus(device) {
  const statusEl = document.getElementById('status' + device.charAt(0).toUpperCase() + device.slice(1));
  if (statusEl) {
    const on = deviceStates[device];
    statusEl.textContent = on ? 'ON' : 'OFF';
    statusEl.className = 'control-status' + (on ? ' on' : '');
  }
}

function toggleOverride() {
  overrideActive = !overrideActive;
  
  const btn = document.getElementById('overrideBtn');
  const banner = document.getElementById('overrideBanner');

  if (overrideActive) {
    // Turn everything off in both rooms
    if (mqttClient?.connected) {
      // Room A
      mqttClient.publish(TOPICS.controlA, JSON.stringify({ device: 'fan', action: 'off', reason: 'emergency_override' }));
      mqttClient.publish(TOPICS.controlA, JSON.stringify({ device: 'lamp', action: 'off', reason: 'emergency_override' }));
      // Room B
      mqttClient.publish(TOPICS.controlB, JSON.stringify({ device: 'fan', action: 'off', reason: 'emergency_override' }));
      mqttClient.publish(TOPICS.controlB, JSON.stringify({ device: 'lamp', action: 'off', reason: 'emergency_override' }));
    }
    
    // Update local states
    ['fanA', 'lampA', 'fanB', 'lampB'].forEach(device => {
      deviceStates[device] = false;
      updateDeviceStatus(device);
    });

    btn.classList.add('active');
    banner.classList.add('active');
    addAlert('danger', '🚨 EMERGENCY OVERRIDE ACTIVATED - All devices disabled');
  } else {
    btn.classList.remove('active');
    banner.classList.remove('active');
    addAlert('info', '✓ Override deactivated - Normal operation restored');
  }
  
  // Update controls state (they should be disabled during override regardless of mode)
  updateControlsState();
}

// ===== ALERTS =====

function handleAlert(data) {
  addAlert('warning', data.message || 'Anomaly detected');
}

function addAlert(type, message) {
  const alertsList = document.getElementById('alerts');
  const time = new Date().toLocaleTimeString('en-US', { hour: '2-digit', minute: '2-digit' });
  
  const icons = {
    info: '<circle cx="12" cy="12" r="10"/><path d="M12 16v-4M12 8h.01"/>',
    warning: '<path d="M12 9v2m0 4h.01m-6.938 4h13.856c1.54 0 2.502-1.667 1.732-3L13.732 4c-.77-1.333-2.694-1.333-3.464 0L3.34 16c-.77 1.333.192 3 1.732 3z"/>',
    danger: '<circle cx="12" cy="12" r="10"/><path d="M15 9l-6 6M9 9l6 6"/>'
  };

  const alertHtml = `
    <div class="alert-item ${type}">
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">${icons[type]}</svg>
      <span>${message}</span>
      <span class="alert-time">${time}</span>
    </div>
  `;

  alertsList.insertAdjacentHTML('afterbegin', alertHtml);

  // Keep only last 10 alerts
  while (alertsList.children.length > 10) {
    alertsList.removeChild(alertsList.lastChild);
  }
}

function clearAlerts() {
  document.getElementById('alerts').innerHTML = '';
  addAlert('info', 'Alerts cleared');
}

// ===== INITIALIZATION =====

// Initialize controls state on page load
document.addEventListener('DOMContentLoaded', () => {
  updateControlsState();
});

connectMQTT();
fetchPricingData();
fetchPriceHistory();

// Update pricing every 5 minutes
setInterval(fetchPricingData, 300000);

// Expose functions globally
window.sendCommand = sendCommand;
window.toggleOverride = toggleOverride;
window.clearAlerts = clearAlerts;
window.setMode = setMode;