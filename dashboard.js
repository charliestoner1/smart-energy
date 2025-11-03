
const BROKER_URL = 'wss://1e1a4e5c581e4bc3a697f8937d7fb9e4.s1.eu.hivemq.cloud:8884/mqtt';


const mqttOptions = {
  username: 'omeravi',          
  password: 'Omeromer1!', 
  keepalive: 60,
  reconnectPeriod: 2000,
  connectTimeout: 10000
};


const TOPICS = {
  telemetry: 'sensors/room1/telemetry',
  control:   'control/room1/cmd',
  alerts:    'alerts/room1/anomaly',
  stateFan:  'control/room1/state/fan',
  stateLamp: 'control/room1/state/lamp'
};


const RATES = [
  { block: 250, rate: 0.110 },
  { block: 500, rate: 0.145 },
  { block: Infinity, rate: 0.185 }
];


let mqttClient = null;
let overrideActive = false;
let accumulatedWh = 0;
let lastSampleTsMs = null;

const deviceStates = { fan: false, lamp: false, pir: false };


const powerData = { labels: [], datasets: [{ label: 'Power (W)', data: [], borderColor: '#00d9ff', tension: 0.35, pointRadius: 0 }] };
const tempData  = {
  labels: [],
  datasets: [
    { label: 'Temp (°C)',    data: [], borderColor: '#e94560', tension: 0.35, pointRadius: 0, yAxisID: 'y'  },
    { label: 'Humidity (%)', data: [], borderColor: '#00d9ff', tension: 0.35, pointRadius: 0, yAxisID: 'y1' }
  ]
};

const powerChart = new Chart(document.getElementById('powerChart'), {
  type: 'line',
  data: powerData,
  options: {
    responsive: true,
    maintainAspectRatio: true,
    animation: false,
    scales: { y: { beginAtZero: true, title: { display: true, text: 'Power (W)' } } },
    plugins: { legend: { display: true } }
  }
});

const tempChart = new Chart(document.getElementById('tempChart'), {
  type: 'line',
  data: tempData,
  options: {
    responsive: true,
    maintainAspectRatio: true,
    animation: false,
    scales: {
      y:  { position: 'left',  title: { display: true, text: 'Temp (°C)' } },
      y1: { position: 'right', title: { display: true, text: 'Humidity (%)' }, grid: { drawOnChartArea: false } }
    },
    plugins: { legend: { display: true } }
  }
});

function addDataPoint(chart, data, label, value) {
  data.labels.push(label);
  if (Array.isArray(value)) {
    value.forEach((v, i) => data.datasets[i].data.push(v));
  } else {
    data.datasets[0].data.push(value);
  }
  const MAX = 60;
  if (data.labels.length > MAX) {
    data.labels.shift();
    data.datasets.forEach(ds => ds.data.shift());
  }
  chart.update('none');
}


function connectMQTT() {
  mqttClient = mqtt.connect(BROKER_URL, mqttOptions);

  mqttClient.on('connect', () => {
    setBrokerStatus(true);
    console.log('✅ Connected to HiveMQ Cloud');
    mqttClient.subscribe([
      TOPICS.telemetry,
      TOPICS.alerts,
      TOPICS.stateFan,
      TOPICS.stateLamp
    ], (err) => { if (err) console.error('Subscribe error', err); });
  });

  mqttClient.on('reconnect', () => setBrokerStatus(false));
  mqttClient.on('close', () => setBrokerStatus(false));
  mqttClient.on('error', (err) => { console.error('MQTT error:', err); setBrokerStatus(false); });

  mqttClient.on('message', (topic, payload) => {
    let data;
    try { data = JSON.parse(payload.toString()); }
    catch { return; } // ignore invalid JSON
    if      (topic === TOPICS.telemetry)  handleTelemetry(data);
    else if (topic === TOPICS.alerts)     handleAlert(data);
    else if (topic === TOPICS.stateFan)   handleStateEcho('fan', data);
    else if (topic === TOPICS.stateLamp)  handleStateEcho('lamp', data);
  });
}

function setBrokerStatus(connected) {
  const el = document.getElementById('statusBroker');
  el.textContent = connected ? 'Broker: Connected' : 'Broker: Disconnected';
  el.classList.toggle('active', connected);
}


function handleTelemetry(d) {
  const tsMs = coerceTsMs(d.ts);
  const timeLabel = new Date(tsMs).toLocaleTimeString();

  const voltage = isFiniteNumber(d.voltage) ? d.voltage : 120;
  const amps    = isFiniteNumber(d.amps) ? d.amps : 0;
  const powerW  = +(voltage * amps).toFixed(1);

  addDataPoint(powerChart, powerData, timeLabel, powerW);

  const t = isFiniteNumber(d.tC) ? +d.tC.toFixed(1) : null;
  const h = isFiniteNumber(d.rh) ? +d.rh.toFixed(1) : null;
  addDataPoint(tempChart, tempData, timeLabel, [t, h]);

  deviceStates.pir = d.pir === 1 || d.pir === true;
  setStatus('PIR', deviceStates.pir, v => v ? 'PIR: Active' : 'PIR: Inactive');

  if (lastSampleTsMs != null && tsMs > lastSampleTsMs) {
    const dtHours = (tsMs - lastSampleTsMs) / 3_600_000;
    accumulatedWh += powerW * dtHours;
  }
  lastSampleTsMs = tsMs;

  const kWh = accumulatedWh / 1000;
  document.getElementById('projectedCost').textContent = costForKWh(kWh).toFixed(2);
}


function handleAlert(d) {
  const msg = d.message || 'Anomaly detected';
  const div = document.getElementById('alerts');
  div.innerHTML = `<div>⚠️ ${escapeHtml(msg)}</div>` + div.innerHTML;
}


function publishCmd(device, action, reason = 'manual') {
  if (!mqttClient || !mqttClient.connected) { alert('Broker not connected yet.'); return; }
  mqttClient.publish(TOPICS.control, JSON.stringify({ device, action, reason }));
}

function sendCommand(device, action) {
  if (overrideActive) {
    alert('Manual override is active. Deactivate to control devices.');
    return;
  }
  publishCmd(device, action, 'manual');
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
}

function handleStateEcho(device, payload) {
  deviceStates[device] = !!payload.on;
  updateDeviceStatus(device);
}

function updateDeviceStatus(device) {
  const id = `status${device.charAt(0).toUpperCase()}${device.slice(1)}`;
  const el = document.getElementById(id);
  const on = !!deviceStates[device];
  el.textContent = `${capitalize(device)}: ${on ? 'ON' : 'OFF'}`;
  el.classList.toggle('active', on);
}


function costForKWh(kWhTotal) {
  let remaining = kWhTotal;
  let cost = 0;
  for (const tier of RATES) {
    const block = Math.min(remaining, tier.block);
    cost += block * tier.rate;
    remaining -= block;
    if (remaining <= 0) break;
  }
  return cost;
}

function setStatus(name, active, textFn) {
  const el = document.getElementById(`status${name}`);
  el.textContent = typeof textFn === 'function'
    ? textFn(active)
    : `${name}: ${active ? 'Active' : 'Inactive'}`;
  el.classList.toggle('active', !!active);
}

function isFiniteNumber(x){ return Number.isFinite(+x); }

function coerceTsMs(ts) {
  const n = Number(ts);
  if (!Number.isFinite(n)) return Date.now();
  if (n > 1e12) return n;        
  if (n > 1e9)  return n * 1000; 
  return Date.now();
}

function escapeHtml(s) {
  return String(s).replace(/[&<>"']/g, m => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[m]));
}

function capitalize(s){ return s ? s[0].toUpperCase()+s.slice(1) : s; }


connectMQTT();


window.sendCommand = sendCommand;
window.toggleOverride = toggleOverride;
