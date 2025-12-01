import { Chart } from "@/components/ui/chart"
// ============================================
// 🔧 CONFIGURATION - UPDATE THESE VALUES
// ============================================
const CONFIG = {
  // Your MQTT broker URL (WebSocket)
  // Examples:
  // - Local: 'ws://192.168.1.100:8000/mqtt'
  // - Cloud: 'wss://your-broker.com:8883/mqtt'
  // - Test broker: 'wss://test.mosquitto.org:8081/mqtt'
  BROKER_URL: "wss://test.mosquitto.org:8081/mqtt",

  // MQTT Topics (from your paper)
  TOPICS: {
    TELEMETRY: "sensors/room1/telemetry",
    CONTROL_CMD: "control/room1/cmd",
    CONTROL_ACK: "control/room1/ack",
    ALERTS: "alerts/room1/anomaly",
  },

  // GRU Tiered Rates (from your paper)
  RATES: [
    { max: 250, rate: 0.11 }, // T1: 0-250 kWh
    { max: 750, rate: 0.145 }, // T2: 251-750 kWh
    { max: Number.POSITIVE_INFINITY, rate: 0.185 }, // T3: >750 kWh
  ],
}

// ============================================
// Global State
// ============================================
let mqttClient = null
let overrideActive = false
let powerChart, tempChart
let cumulativeEnergy = 0 // kWh
let lastTimestamp = Date.now()

// Chart data storage
const chartData = {
  power: { labels: [], data: [] },
  temp: { labels: [], tempData: [], humidData: [] },
}

// MQTT library import
const mqtt = require("mqtt")

// ============================================
// Initialize Dashboard
// ============================================
window.onload = () => {
  initCharts()
  connectMQTT()
}

// ============================================
// Chart Initialization
// ============================================
function initCharts() {
  // Power Chart
  const powerCtx = document.getElementById("powerChart").getContext("2d")
  powerChart = new Chart(powerCtx, {
    type: "line",
    data: {
      labels: [],
      datasets: [
        {
          label: "Power (W)",
          data: [],
          borderColor: "#667eea",
          backgroundColor: "rgba(102, 126, 234, 0.1)",
          tension: 0.4,
          fill: true,
        },
      ],
    },
    options: {
      responsive: true,
      maintainAspectRatio: false,
      scales: {
        y: { beginAtZero: true, title: { display: true, text: "Watts" } },
      },
    },
  })

  // Temperature & Humidity Chart
  const tempCtx = document.getElementById("tempChart").getContext("2d")
  tempChart = new Chart(tempCtx, {
    type: "line",
    data: {
      labels: [],
      datasets: [
        {
          label: "Temperature (°C)",
          data: [],
          borderColor: "#ef4444",
          backgroundColor: "rgba(239, 68, 68, 0.1)",
          yAxisID: "y",
          tension: 0.4,
        },
        {
          label: "Humidity (%)",
          data: [],
          borderColor: "#3b82f6",
          backgroundColor: "rgba(59, 130, 246, 0.1)",
          yAxisID: "y1",
          tension: 0.4,
        },
      ],
    },
    options: {
      responsive: true,
      maintainAspectRatio: false,
      scales: {
        y: { type: "linear", position: "left", title: { display: true, text: "°C" } },
        y1: {
          type: "linear",
          position: "right",
          title: { display: true, text: "%" },
          grid: { drawOnChartArea: false },
        },
      },
    },
  })
}

// ============================================
// MQTT Connection
// ============================================
function connectMQTT() {
  updateConnectionStatus("connecting", "Connecting...")

  try {
    mqttClient = mqtt.connect(CONFIG.BROKER_URL)

    mqttClient.on("connect", () => {
      console.log("[v0] MQTT Connected")
      updateConnectionStatus("connected", "Connected")

      // Subscribe to topics
      mqttClient.subscribe(CONFIG.TOPICS.TELEMETRY, { qos: 1 })
      mqttClient.subscribe(CONFIG.TOPICS.CONTROL_ACK, { qos: 1 })
      mqttClient.subscribe(CONFIG.TOPICS.ALERTS, { qos: 1 })
    })

    mqttClient.on("message", (topic, message) => {
      try {
        const data = JSON.parse(message.toString())
        handleMQTTMessage(topic, data)
      } catch (e) {
        console.error("[v0] Error parsing MQTT message:", e)
      }
    })

    mqttClient.on("error", (error) => {
      console.error("[v0] MQTT Error:", error)
      updateConnectionStatus("disconnected", "Error")
    })

    mqttClient.on("close", () => {
      console.log("[v0] MQTT Disconnected")
      updateConnectionStatus("disconnected", "Disconnected")
    })
  } catch (error) {
    console.error("[v0] Failed to connect:", error)
    updateConnectionStatus("disconnected", "Failed")
  }
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

// ============================================
// Update Telemetry Display
// ============================================
function updateTelemetry(data) {
  const { ts, tC, rh, pir, amps } = data

  // Calculate power (assuming 120V)
  const voltage = 120
  const power = voltage * amps

  // Update energy consumption
  const now = Date.now()
  const hoursPassed = (now - lastTimestamp) / (1000 * 60 * 60)
  cumulativeEnergy += (power / 1000) * hoursPassed
  lastTimestamp = now

  // Update charts
  const timeLabel = new Date(ts * 1000).toLocaleTimeString()
  updateChart(powerChart, chartData.power, timeLabel, power)
  updateChart(tempChart, chartData.temp, timeLabel, tC, rh)

  // Update status badges
  document.getElementById("pirStatus").textContent = pir ? "Active" : "Inactive"
  document.getElementById("pirStatus").className = pir ? "badge active" : "badge inactive"

  // Update projected cost
  updateProjectedCost()
}

// ============================================
// Update Chart Helper
// ============================================
function updateChart(chart, storage, label, ...values) {
  storage.labels.push(label)
  values.forEach((val, idx) => {
    if (!storage.data[idx]) storage.data[idx] = []
    storage.data[idx].push(val)
  })

  // Keep last 20 points
  if (storage.labels.length > 20) {
    storage.labels.shift()
    storage.data.forEach((arr) => arr.shift())
  }

  chart.data.labels = storage.labels
  chart.data.datasets.forEach((dataset, idx) => {
    dataset.data = storage.data[idx] || []
  })
  chart.update("none")
}

// ============================================
// Calculate Projected Cost (GRU Tiered)
// ============================================
function updateProjectedCost() {
  let cost = 0
  let remaining = cumulativeEnergy

  for (const tier of CONFIG.RATES) {
    const tierUsage = Math.min(remaining, tier.max)
    cost += tierUsage * tier.rate
    remaining -= tierUsage
    if (remaining <= 0) break
  }

  // Project to full day
  const hoursElapsed = (Date.now() - lastTimestamp) / (1000 * 60 * 60)
  const dailyProjection = (cost / hoursElapsed) * 24

  document.getElementById("projectedCost").textContent = `$${dailyProjection.toFixed(2)}`
}

// ============================================
// Send Control Command
// ============================================
function sendCommand(device, action) {
  if (overrideActive) {
    alert("Manual override is active. Deactivate to control devices.")
    return
  }

  if (!mqttClient || !mqttClient.connected) {
    alert("MQTT not connected. Cannot send command.")
    return
  }

  const command = {
    device: device,
    action: action,
    reason: "manual",
    timestamp: Math.floor(Date.now() / 1000),
  }

  mqttClient.publish(CONFIG.TOPICS.CONTROL_CMD, JSON.stringify(command), { qos: 1 })

  // Update UI
  const statusId = device + "Status"
  document.getElementById(statusId).textContent = action.toUpperCase()
  document.getElementById(statusId).className = action === "on" ? "badge active" : "badge inactive"
}

// ============================================
// Emergency Override
// ============================================
function toggleOverride() {
  overrideActive = !overrideActive

  const banner = document.getElementById("overrideBanner")
  const btn = document.getElementById("overrideBtn")
  const controls = document.querySelectorAll(".control-btn")

  if (overrideActive) {
    // Activate override - shut down all devices
    banner.style.display = "block"
    btn.textContent = "✅ OVERRIDE ACTIVE - CLICK TO RESTORE"
    btn.classList.add("active")
    controls.forEach((btn) => (btn.disabled = true))

    // Send shutdown commands
    sendCommand("fan", "off")
    sendCommand("lamp", "off")

    document.getElementById("modeSelect").value = "manual"
    document.getElementById("modeSelect").disabled = true
  } else {
    // Deactivate override
    banner.style.display = "none"
    btn.textContent = "🚨 EMERGENCY OVERRIDE"
    btn.classList.remove("active")
    controls.forEach((btn) => (btn.disabled = false))
    document.getElementById("modeSelect").disabled = false
  }
}

// ============================================
// Update Connection Status
// ============================================
function updateConnectionStatus(status, text) {
  const dot = document.querySelector(".status-dot")
  const statusText = document.getElementById("statusText")
  const brokerStatus = document.getElementById("brokerStatus")

  dot.className = `status-dot ${status}`
  statusText.textContent = text
  brokerStatus.textContent = text
  brokerStatus.className = status === "connected" ? "badge active" : "badge inactive"
}

// ============================================
// Add Alert
// ============================================
function addAlert(message) {
  const alertsList = document.getElementById("alertsList")
  const noAlerts = alertsList.querySelector(".no-alerts")
  if (noAlerts) noAlerts.remove()

  const alertDiv = document.createElement("div")
  alertDiv.className = "alert-item"
  alertDiv.textContent = `⚠️ ${message}`
  alertsList.prepend(alertDiv)

  // Keep only last 5 alerts
  while (alertsList.children.length > 5) {
    alertsList.lastChild.remove()
  }
}
