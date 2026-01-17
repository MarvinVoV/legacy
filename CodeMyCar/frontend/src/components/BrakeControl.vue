<template>
  <div class="brake-container">
    <q-btn
      round
      size="md"
      push
      color="negative"
      class="brake-button"
      :class="{ 'pressed': isPressed }"
      label="STOP"
      @mousedown="pressBrake"
      @touchstart="pressBrake"
      @mouseup="releaseBrake"
      @touchend="releaseBrake"
      @mouseleave="releaseBrake"
    />
  </div>
</template>

<script lang="ts">
export default {
  name: 'BrakeControl',
  emits: ['brake-activate'],
  data() {
    return {
      isPressed: false
    }
  },
  methods: {
    pressBrake() {
      this.isPressed = true
      this.$emit('brake-activate')
    },
    releaseBrake() {
      this.isPressed = false
    }
  }
}
</script>

<style scoped>
.brake-container {
  position: absolute;
  bottom: 20px;
  right: 20px;
  display: flex;
  flex-direction: column;
  align-items: center;
  z-index: 10;
}

.brake-button {
  width: 60px;
  height: 60px;
  transition: all 0.1s;
  box-shadow: 0 4px 12px rgba(255, 0, 0, 0.3);
}

.brake-button.pressed {
  transform: scale(0.9);
  box-shadow: 0 2px 6px rgba(255, 0, 0, 0.3);
}

.brake-label {
  margin-top: 8px;
  text-align: center;
}
</style>
