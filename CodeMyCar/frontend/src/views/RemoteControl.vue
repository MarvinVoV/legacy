<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, reactive, ref, watch } from 'vue'
import DrivePad from '@/components/DrivePad.vue'
import { buildControlPayload, buildEmergencyStopPayload, profiles, type ControlVector, type DriveMode, type DriveProfile } from '@/lib/control'
import { CarApi, defaultApiBase, type DeviceStatus, type MotionState } from '@/services/carApi'

type ControlPanel = 'live' | 'mission'
const mockMode = new URLSearchParams(window.location.search).has('mock')
const saved = JSON.parse(localStorage.getItem('cmcar-console') || '{}') as Record<string, string>
const settings = reactive({ baseUrl: saved.baseUrl || defaultApiBase(), apiKey: saved.apiKey || '', deviceId: saved.deviceId || 'DEV_1' })
const api = ref(new CarApi(settings))
const devices = ref<DeviceStatus[]>([])
const status = ref<DeviceStatus | null>(null)
const motion = ref<MotionState | null>(null)
const vector = ref<ControlVector>({ throttle: 0, steering: 0 })
const panel = ref<ControlPanel>('live')
const driveMode = ref<DriveMode>('drive')
const profile = ref<DriveProfile>('standard')
const showSettings = ref(false)
const loading = ref(true)
const commandActive = ref(false)
const commandPending = ref(false)
const missionRunning = ref(false)
const latencyMs = ref<number | null>(null)
const lastError = ref('')
const toast = ref('')
const seq = ref(Math.floor(Math.random() * 200))
const mission = reactive({ distance_cm: 50, turn_deg: 0, speed_cm_s: 25 })
let commandTimer: number | undefined
let statusTimer: number | undefined
let motionTimer: number | undefined
let toastTimer: number | undefined
let mockTimer: number | undefined

const selectedDevice = computed(() => mockMode ? 'DEMO_CAR' : settings.deviceId || devices.value[0]?.device_id || 'DEV_1')
const ready = computed(() => mockMode || Boolean(status.value?.online && status.value?.stm32_connected))
const onlineLabel = computed(() => mockMode ? '演示模式' : loading.value ? '正在连接' : !status.value?.online ? '设备离线' : !status.value?.stm32_connected ? 'STM32 断开' : '系统就绪')
const stateLabel = computed(() => ({ idle: '空闲', running: '运行中', done: '已完成', aborted: '已停止', error: '故障' } as Record<string, string>)[motion.value?.motion.state || ''] || '未知')
const heading = computed(() => motion.value?.pose.heading_deg ?? 0)
const speedNow = computed(() => motion.value?.velocity.linear_cm_s ?? 0)
const frontDistance = computed(() => status.value?.front_distance_cm ?? null)
const rearDistance = computed(() => status.value?.rear_distance_cm ?? null)
const dangerFront = computed(() => frontDistance.value !== null && frontDistance.value < 30)
const selectedProfile = computed(() => profiles[profile.value])
const directionText = computed(() => vector.value.throttle > 0 ? '前进' : vector.value.throttle < 0 ? '倒车' : '待命')
const steeringText = computed(() => vector.value.steering < 0 ? `左 ${Math.abs(vector.value.steering)}%` : vector.value.steering > 0 ? `右 ${vector.value.steering}%` : '居中')

function nextSeq() { seq.value = (seq.value + 1) & 0xff; return seq.value }
function notify(message: string) { toast.value = message; window.clearTimeout(toastTimer); toastTimer = window.setTimeout(() => { toast.value = '' }, 2400) }
function describeError(error: unknown) {
  const message = error instanceof Error ? error.message : String(error)
  return /failed to fetch|networkerror|load failed/i.test(message)
    ? '无法连接 Pyserver，请检查服务地址或运行状态'
    : message
}

function updateMockTelemetry() {
  const now = Date.now() / 1000
  const linear = vector.value.throttle * 0.5
  status.value = {
    device_id: 'DEMO_CAR', online: true, last_heartbeat_age: 0.2, uptime_s: 7420,
    heap_free: 281344, stm32_connected: true,
    front_distance_cm: Math.round(118 + Math.sin(now * 0.7) * 24),
    rear_distance_cm: Math.round(186 + Math.cos(now * 0.5) * 30), sensor_age_s: 0.1,
  }
  motion.value = {
    ok: true, online: true, stm32_connected: true, motion_supported: true,
    motion: { state: commandActive.value ? 'running' : 'idle', seq: seq.value, age_ms: 80, stale: false },
    pose: {
      x_cm: 128.4 + Math.sin(now * 0.08) * 2,
      y_cm: 36.8,
      heading_deg: vector.value.steering * 0.22,
      odometer_cm: 842.6,
    },
    velocity: { linear_cm_s: linear, angular_deg_s: vector.value.steering * 0.35 },
  }
}

async function refreshDevices() {
  if (mockMode) return
  try {
    const list = await api.value.listDevices(); devices.value = list
    if (!list.some((item) => item.device_id === settings.deviceId) && list.length) settings.deviceId = list.find((item) => item.online)?.device_id || list[0].device_id
    lastError.value = ''
  } catch (error) { lastError.value = describeError(error) }
}
async function refreshStatus() {
  if (mockMode) { updateMockTelemetry(); return }
  try { status.value = await api.value.getDeviceStatus(selectedDevice.value); loading.value = false; lastError.value = '' }
  catch (error) { loading.value = false; lastError.value = describeError(error); if (status.value) status.value = { ...status.value, online: false } }
}
async function refreshMotion() {
  if (mockMode) { updateMockTelemetry(); return }
  try { motion.value = await api.value.getMotionState(selectedDevice.value) } catch { /* status polling owns the visible error */ }
}

async function transmitControl() {
  if (!commandActive.value || commandPending.value || !ready.value) return
  if (mockMode) { nextSeq(); updateMockTelemetry(); return }
  commandPending.value = true
  try { await api.value.sendControl(selectedDevice.value, buildControlPayload(nextSeq(), driveMode.value, profile.value, vector.value)); lastError.value = '' }
  catch (error) { lastError.value = describeError(error); await emergencyStop(false) }
  finally { commandPending.value = false }
}
function engageControl() {
  if (!ready.value || commandActive.value) return
  commandActive.value = true; void transmitControl(); window.clearInterval(commandTimer); commandTimer = window.setInterval(() => void transmitControl(), 180)
}
function stopCommandLoop() { commandActive.value = false; window.clearInterval(commandTimer); commandTimer = undefined }
async function emergencyStop(showFeedback = true) {
  stopCommandLoop(); vector.value = { throttle: 0, steering: 0 }
  if (mockMode) {
    updateMockTelemetry()
    if (showFeedback) { navigator.vibrate?.([35, 35, 70]); notify('演示制动已触发') }
    return
  }
  const deviceId = selectedDevice.value
  const results = await Promise.allSettled([
    api.value.sendControl(deviceId, buildEmergencyStopPayload(nextSeq())),
    api.value.stop(deviceId),
  ])
  if (showFeedback) { navigator.vibrate?.([35, 35, 70]); notify(results.some((result) => result.status === 'fulfilled') ? '制动命令已送达' : '制动发送失败') }
  void refreshMotion()
}
function releaseControl() { if (commandActive.value) void emergencyStop(false) }
async function runMission() {
  if (!ready.value || missionRunning.value) return
  missionRunning.value = true; lastError.value = ''
  try {
    if (mockMode) {
      if (motion.value) motion.value.motion.state = 'running'
      await new Promise((resolve) => window.setTimeout(resolve, 1600))
      if (motion.value) {
        motion.value.motion.state = 'done'
        motion.value.pose.x_cm += mission.distance_cm
        motion.value.pose.heading_deg += mission.turn_deg
        motion.value.pose.odometer_cm += Math.abs(mission.distance_cm)
      }
    } else await api.value.move(selectedDevice.value, { ...mission })
    notify('任务已完成')
  }
  catch (error) { lastError.value = describeError(error); notify('任务未完成') }
  finally { missionRunning.value = false; if (!mockMode) void refreshMotion() }
}
async function ping() {
  if (mockMode) { latencyMs.value = 12; notify('演示链路 12 ms'); return }
  try { latencyMs.value = await api.value.ping(selectedDevice.value); notify(`链路 ${latencyMs.value} ms`) }
  catch (error) { lastError.value = describeError(error); notify('链路检测失败') }
}
function saveSettings() {
  api.value.updateConfig(settings); localStorage.setItem('cmcar-console', JSON.stringify(settings)); showSettings.value = false; loading.value = true
  void refreshDevices().then(refreshStatus); void refreshMotion(); notify('连接设置已保存')
}
function changePanel(next: ControlPanel) { if (panel.value !== next) { if (commandActive.value) void emergencyStop(false); panel.value = next } }
function handleVisibility() { if (document.hidden && (commandActive.value || missionRunning.value)) void emergencyStop(false) }

watch(selectedDevice, () => { status.value = null; motion.value = null; void refreshStatus(); void refreshMotion() })
onMounted(async () => {
  if (mockMode) {
    loading.value = false
    devices.value = [{ device_id: 'DEMO_CAR', online: true, last_heartbeat_age: 0.2, uptime_s: 7420, heap_free: 281344, stm32_connected: true, front_distance_cm: 118, rear_distance_cm: 186, sensor_age_s: 0.1 }]
    updateMockTelemetry()
    mockTimer = window.setInterval(updateMockTelemetry, 500)
    document.addEventListener('visibilitychange', handleVisibility); window.addEventListener('pagehide', handleVisibility)
    return
  }
  await refreshDevices(); await Promise.all([refreshStatus(), refreshMotion()])
  statusTimer = window.setInterval(() => void refreshStatus(), 1600); motionTimer = window.setInterval(() => void refreshMotion(), 550)
  document.addEventListener('visibilitychange', handleVisibility); window.addEventListener('pagehide', handleVisibility)
})
onBeforeUnmount(() => {
  if (commandActive.value || missionRunning.value) void emergencyStop(false)
  window.clearInterval(statusTimer); window.clearInterval(motionTimer); window.clearInterval(commandTimer); window.clearTimeout(toastTimer)
  window.clearInterval(mockTimer)
  document.removeEventListener('visibilitychange', handleVisibility); window.removeEventListener('pagehide', handleVisibility)
})
</script>

<template>
  <main class="console-shell" :style="{ '--profile-accent': selectedProfile.accent }">
    <div class="ambient ambient-one"></div><div class="ambient ambient-two"></div>
    <header class="topbar">
      <div class="brand-lockup">
        <div class="brand-mark"><svg viewBox="0 0 36 36"><path d="M6 22V14l4-5h16l4 5v8l-3 2H9zM11 24v3M25 24v3M10 17h16M13 13h10" /></svg></div>
        <div><div class="eyebrow">CODE MY CAR</div><h1>Drive Console</h1></div>
      </div>
      <div class="top-actions">
        <button class="connection-pill" :class="{ online: ready }" @click="ping"><span class="pulse-dot"></span><span>{{ onlineLabel }}</span><small v-if="latencyMs !== null">{{ latencyMs }}ms</small></button>
        <button class="icon-button" aria-label="连接设置" @click="showSettings = true"><svg viewBox="0 0 24 24"><path d="M12 15.5a3.5 3.5 0 1 0 0-7 3.5 3.5 0 0 0 0 7Zm7.4-1.5a7.8 7.8 0 0 0 .1-2l2-1.5-2-3.5-2.5 1a8 8 0 0 0-1.7-1L15 4.3h-4L10.7 7a8 8 0 0 0-1.7 1L6.5 7l-2 3.5 2 1.5a8 8 0 0 0 .1 2l-2.1 1.5 2 3.5 2.5-1a8 8 0 0 0 1.7 1l.3 2.7h4l.3-2.7a8 8 0 0 0 1.7-1l2.5 1 2-3.5Z" /></svg></button>
      </div>
    </header>

    <section class="status-rail" aria-label="车辆状态">
      <article class="telemetry-card primary-stat"><div class="stat-icon"><svg viewBox="0 0 24 24"><path d="M4 17a8 8 0 1 1 16 0M12 13l4-4M7 17h10" /></svg></div><div><span>实时速度</span><strong>{{ speedNow.toFixed(1) }}</strong><small>cm/s</small></div></article>
      <article class="telemetry-card"><div class="stat-icon"><svg viewBox="0 0 24 24"><path d="M12 3l4 8-4 10-4-10 4-8Zm0 8h4" /></svg></div><div><span>航向</span><strong>{{ heading.toFixed(0) }}°</strong><small>{{ stateLabel }}</small></div></article>
      <article class="telemetry-card" :class="{ warning: dangerFront }"><div class="stat-icon"><svg viewBox="0 0 24 24"><path d="M4 8v8m4-6v4m4-2h8M3 5c5 3 5 11 0 14" /></svg></div><div><span>前方距离</span><strong>{{ frontDistance ?? '—' }}</strong><small>cm</small></div></article>
      <article class="telemetry-card"><div class="stat-icon"><svg viewBox="0 0 24 24"><path d="M20 8v8m-4-6v4m-4-2H4m17-7c-5 3-5 11 0 14" /></svg></div><div><span>后方距离</span><strong>{{ rearDistance ?? '—' }}</strong><small>cm</small></div></article>
    </section>

    <div v-if="lastError" class="error-strip"><svg viewBox="0 0 24 24"><path d="M12 3 2.5 20h19L12 3Zm0 6v5m0 3v.1" /></svg><span>{{ lastError }}</span><button @click="lastError = ''">×</button></div>

    <section class="control-stage">
      <div class="control-header">
        <div class="segmented-tabs" role="tablist">
          <button :class="{ active: panel === 'live' }" @click="changePanel('live')"><svg viewBox="0 0 24 24"><path d="M4 15h16M7 15l2-4h6l2 4M8 18h.1M16 18h.1" /></svg>实时驾驶</button>
          <button :class="{ active: panel === 'mission' }" @click="changePanel('mission')"><svg viewBox="0 0 24 24"><path d="M5 20V5l7-2 7 2v15l-7-2-7 2Zm7-17v15M8 9l2 2m6-3-2 2" /></svg>路径任务</button>
        </div>
        <div class="device-chip"><span>{{ selectedDevice }}</span><i :class="{ on: ready }"></i></div>
      </div>

      <div v-if="panel === 'live'" class="live-grid">
        <aside class="control-sidebar">
          <div class="sidebar-block"><div class="block-label">控制协议</div><div class="mode-switch">
            <button :class="{ active: driveMode === 'drive' }" @click="driveMode = 'drive'"><strong>DRIVE</strong><span>速度 + 转向</span></button>
            <button :class="{ active: driveMode === 'direct' }" @click="driveMode = 'direct'"><strong>DIRECT</strong><span>轮速直控</span></button>
          </div></div>
          <div class="sidebar-block"><div class="block-label">速度上限</div><div class="profile-list">
            <button v-for="(item, key) in profiles" :key="key" :class="{ active: profile === key }" @click="profile = key">
              <span class="profile-dot" :style="{ background: item.accent }"></span><span><strong>{{ item.label }}</strong><small>{{ item.maxMmS / 10 }} cm/s</small></span><svg viewBox="0 0 24 24"><path d="m9 18 6-6-6-6" /></svg>
            </button>
          </div></div>
        </aside>

        <div class="pad-column">
          <div class="readout-row">
            <div class="drive-readout"><span>THROTTLE</span><strong>{{ Math.abs(vector.throttle) }}%</strong><small>{{ directionText }}</small></div>
            <div class="center-readout"><span class="hold-indicator" :class="{ active: commandActive }"></span><p>{{ commandActive ? '保持触控 · 指令续租中' : '按住并拖动开始驾驶' }}</p></div>
            <div class="drive-readout align-right"><span>STEERING</span><strong>{{ Math.abs(vector.steering) }}%</strong><small>{{ steeringText }}</small></div>
          </div>
          <DrivePad v-model="vector" :disabled="!ready" @engage="engageControl" @release="releaseControl" />
          <div class="pad-caption"><span><i class="swipe vertical"></i>上下控制速度</span><span><i class="swipe horizontal"></i>左右控制转向</span><span><i class="release-dot"></i>松手自动制动</span></div>
        </div>

        <aside class="brake-column">
          <button class="brake-button" @click="emergencyStop(true)"><span class="brake-rings"></span><span class="brake-core"><svg viewBox="0 0 24 24"><path d="M7 7h10v10H7z" /></svg><strong>STOP</strong><small>紧急制动</small></span></button>
          <div class="safety-copy"><span class="shield"><svg viewBox="0 0 24 24"><path d="M12 3 5 6v5c0 5 3 8 7 10 4-2 7-5 7-10V6l-7-3Zm-3 9 2 2 4-5" /></svg></span><div><strong>安全租约已启用</strong><small>失联自动停车</small></div></div>
        </aside>
      </div>

      <div v-else class="mission-panel">
        <div class="mission-visual"><div class="route-map"><span class="route-line"></span><span class="route-origin">START</span><span class="route-car"><svg viewBox="0 0 36 36"><path d="M6 22V14l4-5h16l4 5v8l-3 2H9zM11 24v3M25 24v3M10 17h16" /></svg></span><span class="route-target">TARGET</span></div><h2>相对路径任务</h2><p>输入距离与航向变化，车辆将以事务式 GOAL 执行并返回实测结果。</p></div>
        <form class="mission-form" @submit.prevent="runMission">
          <label><span>行驶距离 <small>正前进 / 负后退</small></span><div class="field"><input v-model.number="mission.distance_cm" type="number" min="-500" max="500" required><b>cm</b></div></label>
          <label><span>航向变化 <small>正右转 / 负左转</small></span><div class="field"><input v-model.number="mission.turn_deg" type="number" min="-360" max="360" required><b>°</b></div></label>
          <label><span>路径速度</span><div class="range-field"><input v-model.number="mission.speed_cm_s" type="range" min="1" max="60"><b>{{ mission.speed_cm_s }} cm/s</b></div></label>
          <button class="mission-start" :disabled="!ready || missionRunning"><span v-if="missionRunning" class="spinner"></span><svg v-else viewBox="0 0 24 24"><path d="m8 5 11 7-11 7V5Z" /></svg>{{ missionRunning ? '任务执行中' : '启动任务' }}</button>
          <button v-if="missionRunning" type="button" class="mission-stop" @click="emergencyStop(true)">立即停止</button>
        </form>
      </div>
    </section>

    <footer class="system-footer"><span><i :class="{ on: status?.online }"></i>ESP32 {{ status?.online ? 'ONLINE' : 'OFFLINE' }}</span><span><i :class="{ on: status?.stm32_connected }"></i>STM32 {{ status?.stm32_connected ? 'LINKED' : 'UNLINKED' }}</span><span class="footer-spacer"></span><span>里程 {{ motion?.pose.odometer_cm?.toFixed(1) ?? '—' }} cm</span><span>Uptime {{ status?.uptime_s ? Math.floor(status.uptime_s / 60) : 0 }} min</span></footer>

    <div v-if="showSettings" class="modal-backdrop" @click.self="showSettings = false"><section class="settings-sheet" role="dialog" aria-modal="true" aria-label="连接设置">
      <div class="sheet-handle"></div><div class="sheet-header"><div><span>CONNECTION</span><h2>车辆连接设置</h2></div><button @click="showSettings = false">×</button></div>
      <label><span>Pyserver 地址</span><input v-model.trim="settings.baseUrl" type="url" placeholder="http://192.168.1.10:8000"></label>
      <label><span>设备</span><select v-model="settings.deviceId"><option v-for="device in devices" :key="device.device_id" :value="device.device_id">{{ device.device_id }} · {{ device.online ? '在线' : '离线' }}</option><option v-if="!devices.length" value="DEV_1">DEV_1</option></select></label>
      <label><span>API Key <small>未启用时留空</small></span><input v-model="settings.apiKey" type="password" autocomplete="off" placeholder="••••••••"></label>
      <p class="settings-hint">手机与电脑需连接同一网络。默认自动使用当前页面主机的 8000 端口。</p><button class="save-button" @click="saveSettings">保存并重新连接</button>
    </section></div>
    <Transition name="toast"><div v-if="toast" class="toast-message">{{ toast }}</div></Transition>
  </main>
</template>
