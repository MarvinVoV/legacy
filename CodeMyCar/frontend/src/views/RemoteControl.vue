<template>
  <q-page class="flex column" style="height: 100%; padding: 15px;">
    <!-- 状态栏 -->
    <div class="q-mb-md">
      <q-card class="status-card">
        <q-card-section class="q-pa-sm">
          <div class="row justify-between text-caption">
            <div>状态: <span>{{ status }}</span></div>
            <div>模式: <span>{{ currentModeLabel }}</span></div>
            <div>电量: <span>{{ battery }}%</span></div>
          </div>
        </q-card-section>
      </q-card>
    </div>

    <!-- 模式选择器 (使用 Quasar Toggle Button) -->
    <div class="q-pa-sm  q-gutter-sm">
      <q-btn-toggle
        v-model="activeMode"
        spread
        no-caps
        push
        glossy
        toggle-color="primary"
        :options="modeOptions"
        @update:model-value="changeMode"
       name="mode"/>
    </div>

    <!-- 主控制区域 -->
    <div class="col">
      <div class="row full-height" style="gap: 15px;">
        <!-- 转向控制 -->
        <q-card class="col control-panel">
<!--          <q-card-section class="text-center q-pb-sm">-->
<!--            转向控制-->
<!--          </q-card-section>-->
          <q-card-section class="fit">
            <SteeringControl
              :steering-angle="steeringAngle"
              @steering-change="updateSteering"
            />
          </q-card-section>
        </q-card>

        <!-- 移动控制 -->
        <q-card class="col control-panel relative-position">
<!--          <q-card-section class="text-center text-subtitle1 q-pb-sm">-->
<!--            速度控制-->
<!--          </q-card-section>-->
          <q-card-section class="fit">
            <SpeedControl
              :speed="speed"
              @speed-change="updateSpeed"
            />
          </q-card-section>
          <!-- 刹车控制悬浮在右下角 -->
          <BrakeControl @brake-activate="activateBrake" />
        </q-card>
      </div>
    </div>
  </q-page>
</template>

<script lang="ts">
import SteeringControl from '@/components/SteeringControl.vue'
import SpeedControl from '@/components/SpeedControl.vue'
import BrakeControl from '@/components/BrakeControl.vue'

export default {
  name: 'RemoteControl',
  components: {
    SteeringControl,
    SpeedControl,
    BrakeControl
  },
  data() {
    return {
      status: '已连接',
      battery: 85,
      activeMode: 'direct',
      steeringAngle: 0,
      speed: 0,
      modes: [
        { id: 'direct', label: '直接控制' },
        { id: 'spin', label: '自旋模式'},
        { id: 'differential', label: '差速模式' }
      ]
    }
  },
  computed: {
    currentModeLabel() {
      const mode = this.modes.find(m => m.id === this.activeMode)
      return mode ? mode.label : '未知模式'
    },
    modeOptions() {
      return this.modes.map(mode => ({
        label: mode.label,
        value: mode.id,
        icon: mode.icon
      }))
    }
  },
  methods: {
    changeMode(modeId) {
      this.activeMode = modeId
      console.log(`切换到模式: ${modeId}`)
    },
    updateSteering(angle) {
      this.steeringAngle = angle
      this.sendControlData()
    },
    updateSpeed(speed) {
      this.speed = speed
      this.sendControlData()
    },
    activateBrake() {
      // 紧急刹车逻辑
      this.steeringAngle = 0
      this.speed = 0
      console.log('紧急刹车!')
      this.sendControlData()
    },
    sendControlData() {
      // 发送控制数据到小车
      console.log(`控制数据 - 转向: ${this.steeringAngle.toFixed(1)}°, 速度: ${this.speed}, 模式: ${this.activeMode}`)
    }
  },
  mounted() {
    // 模拟连接状态变化
    setInterval(() => {
      const statuses = ['已连接', '信号弱', '连接中...']
      this.status = statuses[Math.floor(Math.random() * statuses.length)]
    }, 5000)
  }
}
</script>

<style scoped>
.status-card {
  background: rgba(0, 0, 0, 0.2);
  border-radius: 20px;
}

.control-panel {
  background: rgba(0, 0, 0, 0.15);
  border-radius: 20px;
  box-shadow: 0 5px 15px rgba(0, 0, 0, 0.2);
}

@media (orientation: portrait) {
  .row.full-height {
    flex-direction: column;
  }
}
</style>
