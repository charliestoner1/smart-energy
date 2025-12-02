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
  telemetry: 'sensors/room1/telemetry',
  control: 'control/room1/cmd',
  alerts: 'alerts/room1/anomaly'
};

// State
let mqttClient = null;
let overrideActive = false;
let accumulatedWh = 0;
let lastSampleTsMs = null;
let currentPricingData = null;

const deviceStates = {
  fanA: false, lampA: false, pirA: false,
  fanB: false, lampB: false, pirB: false,
  ecoMode: true
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

// MQTT Connection
function connectMQTT() {
  mqttClient = mqtt.connect(BROKER_URL, mqttOptions);

  mqttClient.on('connect', () => {
    updateConnectionStatus(true);
    mqttClient.subscribe([TOPICS.telemetry, TOPICS.alerts]);
    addAlert('info', 'Connected to MQTT broker');
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
      if (topic === TOPICS.telemetry) handleTelemetry(data);
      else if (topic === TOPICS.alerts) handleAlert(data);
    } catch (e) { /* ignore invalid JSON */ }
  });
}

function updateConnectionStatus(connected) {
  const el = document.getElementById('connectionStatus');
  el.className = 'connection-status' + (connected ? ' connected' : '');
  el.querySelector('span:last-child').textContent = connected ? 'Connected' : 'Connecting...';
}

// Telemetry handling
function handleTelemetry(d) {
  const tsMs = d.ts > 1e12 ? d.ts : d.ts * 1000;
  const timeLabel = new Date(tsMs).toLocaleTimeString('en-US', { hour: '2-digit', minute: '2-digit' });

  // Power calculation
  const voltage = d.voltage || 120;
  const amps = d.amps || 0;
  const powerW = +(voltage * amps).toFixed(1);
  addDataPoint(powerChart, timeLabel, powerW);

  // Temperature (Room A and B)
  const tempA = d.tC_A !== undefined ? d.tC_A : (d.tC || null);
  const tempB = d.tC_B !== undefined ? d.tC_B : null;
  
  if (tempA !== null) {
    document.getElementById('tempA').textContent = tempA.toFixed(1) + '°C';
    tempChart.data.datasets[0].data.push(tempA);
  }
  if (tempB !== null) {
    document.getElementById('tempB').textContent = tempB.toFixed(1) + '°C';
    tempChart.data.datasets[1].data.push(tempB);
  }
  tempChart.data.labels.push(timeLabel);
  trimChartData(tempChart, 60);
  tempChart.update('none');

  // Humidity
  if (d.rh_A !== undefined) document.getElementById('humidityA').textContent = d.rh_A.toFixed(0) + '%';
  if (d.rh_B !== undefined) document.getElementById('humidityB').textContent = d.rh_B.toFixed(0) + '%';
  if (d.rh !== undefined) {
    document.getElementById('humidityA').textContent = d.rh.toFixed(0) + '%';
  }

  // Occupancy
  updateOccupancy('A', d.pirA || d.pir);
  updateOccupancy('B', d.pirB);

  // Mode
  if (d.ecoMode !== undefined) {
    deviceStates.ecoMode = d.ecoMode;
    updateModeDisplay();
  }

  // Cost calculation
  if (lastSampleTsMs != null && tsMs > lastSampleTsMs) {
    const dtHours = (tsMs - lastSampleTsMs) / 3600000;
    accumulatedWh += powerW * dtHours;
  }
  lastSampleTsMs = tsMs;
  updateProjectedCost();
}

function updateOccupancy(room, occupied) {
  const badge = document.getElementById('occupancy' + room);
  const card = document.getElementById('room' + room);
  
  if (occupied) {
    badge.className = 'occupancy-badge active';
    badge.querySelector('span:last-child').textContent = 'Occupied';
    card.classList.add('occupied');
  } else {
    badge.className = 'occupancy-badge';
    badge.querySelector('span:last-child').textContent = 'Vacant';
    card.classList.remove('occupied');
  }
}

function updateModeDisplay() {
  const badge = document.getElementById('modeBadge');
  if (deviceStates.ecoMode) {
    badge.className = 'mode-badge';
    badge.innerHTML = '<span class="mode-icon">⚡</span><span class="mode-text">ECO MODE</span>';
  } else {
    badge.className = 'mode-badge manual';
    badge.innerHTML = '<span class="mode-icon">🔧</span><span class="mode-text">MANUAL MODE</span>';
  }
}

// Chart helpers
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

// Pricing
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

// Controls
function sendCommand(device, action) {
  if (overrideActive) {
    addAlert('warning', 'Manual override is active. Deactivate to control devices.');
    return;
  }

  if (!mqttClient?.connected) {
    addAlert('warning', 'Not connected to broker');
    return;
  }

  mqttClient.publish(TOPICS.control, JSON.stringify({ device, action, reason: 'manual' }));
  
  // Update local state
  deviceStates[device] = action === 'on';
  updateDeviceStatus(device);
  addAlert('info', `${device} turned ${action.toUpperCase()}`);
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
    // Turn everything off
    ['fanA', 'lampA', 'fanB', 'lampB'].forEach(device => {
      if (mqttClient?.connected) {
        mqttClient.publish(TOPICS.control, JSON.stringify({ device, action: 'off', reason: 'override' }));
      }
      deviceStates[device] = false;
      updateDeviceStatus(device);
    });

    btn.classList.add('active');
    banner.classList.add('active');
    addAlert('danger', 'EMERGENCY OVERRIDE ACTIVATED - All devices disabled');
  } else {
    btn.classList.remove('active');
    banner.classList.remove('active');
    addAlert('info', 'Override deactivated - Normal operation restored');
  }
}

// Alerts
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

// Initialize
connectMQTT();
fetchPricingData();
fetchPriceHistory();

// Update pricing every 5 minutes
setInterval(fetchPricingData, 300000);

// Expose functions globally
window.sendCommand = sendCommand;
window.toggleOverride = toggleOverride;
window.clearAlerts = clearAlerts;