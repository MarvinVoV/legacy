<script setup lang="ts">
import { computed, ref } from 'vue'
import { pointToControl, type ControlVector } from '@/lib/control'

const props = defineProps<{
  modelValue: ControlVector
  disabled?: boolean
}>()

const emit = defineEmits<{
  'update:modelValue': [value: ControlVector]
  engage: []
  release: []
}>()

const pad = ref<HTMLElement | null>(null)
const pointerId = ref<number | null>(null)
const active = computed(() => pointerId.value !== null)

const knobStyle = computed(() => ({
  left: `${50 + props.modelValue.steering * 0.38}%`,
  top: `${50 - props.modelValue.throttle * 0.38}%`,
}))

function updatePointer(event: PointerEvent) {
  if (!pad.value || props.disabled) return
  emit('update:modelValue', pointToControl(event.clientX, event.clientY, pad.value.getBoundingClientRect()))
}

function onPointerDown(event: PointerEvent) {
  if (props.disabled || !pad.value) return
  pointerId.value = event.pointerId
  pad.value.setPointerCapture(event.pointerId)
  updatePointer(event)
  emit('engage')
  navigator.vibrate?.(12)
}

function onPointerMove(event: PointerEvent) {
  if (pointerId.value !== event.pointerId) return
  updatePointer(event)
}

function finish(event?: PointerEvent) {
  if (event && pointerId.value !== event.pointerId) return
  pointerId.value = null
  emit('update:modelValue', { throttle: 0, steering: 0 })
  emit('release')
}
</script>

<template>
  <div
    ref="pad"
    class="drive-pad"
    :class="{ active, disabled }"
    role="slider"
    aria-label="速度与转向控制摇杆"
    :aria-valuetext="`速度 ${modelValue.throttle}%，转向 ${modelValue.steering}%`"
    tabindex="0"
    @pointerdown.prevent="onPointerDown"
    @pointermove.prevent="onPointerMove"
    @pointerup.prevent="finish"
    @pointercancel.prevent="finish"
    @lostpointercapture="finish"
  >
    <div class="pad-grid" aria-hidden="true">
      <span class="axis axis-x"></span>
      <span class="axis axis-y"></span>
      <span class="orbit orbit-one"></span>
      <span class="orbit orbit-two"></span>
    </div>

    <div class="pad-label pad-forward">前进</div>
    <div class="pad-label pad-reverse">倒车</div>
    <div class="pad-label pad-left">左</div>
    <div class="pad-label pad-right">右</div>

    <div class="pad-knob" :style="knobStyle">
      <div class="knob-core">
        <svg viewBox="0 0 24 24" aria-hidden="true">
          <path d="M12 3v18M3 12h18M8 7l4-4 4 4M8 17l4 4 4-4M7 8l-4 4 4 4M17 8l4 4-4 4" />
        </svg>
      </div>
    </div>

    <div v-if="disabled" class="pad-lock">
      <svg viewBox="0 0 24 24" aria-hidden="true"><path d="M7 11V8a5 5 0 0 1 10 0v3m-11 0h12v10H6z" /></svg>
      <span>等待车辆连接</span>
    </div>
  </div>
</template>

<style scoped>
.drive-pad {
  position: relative;
  width: min(100%, 410px);
  aspect-ratio: 1.25 / 1;
  overflow: hidden;
  touch-action: none;
  border: 1px solid rgba(122, 232, 255, 0.16);
  border-radius: 32px;
  background:
    radial-gradient(circle at 50% 50%, rgba(64, 213, 255, 0.1), transparent 45%),
    linear-gradient(145deg, rgba(16, 29, 40, 0.94), rgba(5, 12, 18, 0.98));
  box-shadow: inset 0 1px rgba(255,255,255,.05), inset 0 -30px 80px rgba(0,0,0,.3), 0 24px 60px rgba(0,0,0,.28);
  user-select: none;
}

.drive-pad::after {
  content: '';
  position: absolute;
  inset: 0;
  pointer-events: none;
  background: linear-gradient(110deg, transparent 30%, rgba(255,255,255,.035) 48%, transparent 65%);
}

.drive-pad.active {
  border-color: rgba(93, 231, 255, .42);
  box-shadow: inset 0 0 50px rgba(51, 212, 255, .09), 0 0 0 1px rgba(71, 217, 255, .08), 0 26px 70px rgba(0,0,0,.36);
}

.pad-grid, .axis, .orbit { position: absolute; pointer-events: none; }
.pad-grid { inset: 15%; }
.axis-x { left: 0; right: 0; top: 50%; height: 1px; background: linear-gradient(90deg, transparent, rgba(111, 221, 244,.22), transparent); }
.axis-y { top: 0; bottom: 0; left: 50%; width: 1px; background: linear-gradient(transparent, rgba(111, 221, 244,.22), transparent); }
.orbit { left: 50%; top: 50%; transform: translate(-50%, -50%); border: 1px solid rgba(123, 227, 248,.11); border-radius: 50%; }
.orbit-one { width: 45%; aspect-ratio: 1; }
.orbit-two { width: 78%; aspect-ratio: 1; }

.pad-label { position: absolute; z-index: 2; color: rgba(187, 215, 224, .48); font-size: 10px; font-weight: 700; letter-spacing: .22em; }
.pad-forward { top: 8%; left: 50%; transform: translateX(-50%); }
.pad-reverse { bottom: 8%; left: 50%; transform: translateX(-50%); }
.pad-left { left: 6%; top: 50%; transform: translateY(-50%); }
.pad-right { right: 6%; top: 50%; transform: translateY(-50%); }

.pad-knob { position: absolute; z-index: 3; width: 76px; height: 76px; transform: translate(-50%, -50%); transition: left 70ms linear, top 70ms linear; }
.drive-pad:not(.active) .pad-knob { transition: left 180ms cubic-bezier(.2,.9,.3,1.25), top 180ms cubic-bezier(.2,.9,.3,1.25); }
.knob-core { width: 100%; height: 100%; display: grid; place-items: center; border-radius: 50%; background: linear-gradient(145deg, #d6fbff, #79e3f4 55%, #2ca7c4); border: 1px solid rgba(255,255,255,.75); box-shadow: 0 12px 30px rgba(29, 204, 240, .28), inset 0 2px 4px rgba(255,255,255,.8), inset 0 -5px 12px rgba(8,94,115,.24); }
.knob-core::before { content: ''; position: absolute; inset: 8px; border: 1px solid rgba(5, 64, 79, .18); border-radius: 50%; }
.knob-core svg { width: 28px; fill: none; stroke: #0b5669; stroke-width: 1.6; stroke-linecap: round; stroke-linejoin: round; }

.pad-lock { position: absolute; z-index: 5; inset: 0; display: flex; flex-direction: column; align-items: center; justify-content: center; gap: 10px; color: #89a2aa; background: rgba(4,10,15,.64); backdrop-filter: blur(5px); font-size: 12px; letter-spacing: .1em; }
.pad-lock svg { width: 25px; fill: none; stroke: currentColor; stroke-width: 1.5; }
.disabled { cursor: not-allowed; }

@media (max-height: 690px) and (orientation: portrait) {
  .drive-pad { aspect-ratio: 1.55 / 1; }
  .pad-knob { width: 66px; height: 66px; }
}
</style>
