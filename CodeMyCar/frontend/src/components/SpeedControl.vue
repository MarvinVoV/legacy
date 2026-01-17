<template>
  <div class="fit column items-center justify-center">
    <div class="q-pa-md" style="width: 80px; height: 220px">
      <div class="row justify-around" style="left: 40px; width: 40px; height: 100%">
        <q-slider
          v-model="speedValue"
          :min="-100"
          :max="100"
          :step="1"
          vertical
          reverse
          label
          label-always
          color="orange"
          track-color="red"
          class="col"
          thumb-size="35px"
          @update:model-value="handleSpeedChange"
          name="speed"
        />
      </div>
    </div>
  </div>
</template>

<script lang="ts">
export default {
  name: 'SpeedControl',
  props: {
    speed: {
      type: Number,
      default: 0,
    },
  },
  emits: ['speed-change'],
  data() {
    return {
      speedValue: 0,
    }
  },
  watch: {
    speed: {
      handler(newVal) {
        this.speedValue = newVal
      },
      immediate: true,
    },
  },
  methods: {
    handleSpeedChange(value) {
      this.$emit('speed-change', value)
    },
  },
}
</script>

<!--<style scoped>-->
<!--:deep(.q-slider) {-->
<!--  width: 10px;-->
<!--}-->

<!--:deep(.q-slider__track) {-->
<!--  background: rgba(255, 255, 255, 0.2);-->
<!--  border-radius: 5px;-->
<!--}-->

<!--:deep(.q-slider__thumb) {-->
<!--  width: 60px;-->
<!--  height: 60px;-->
<!--  background: linear-gradient(135deg, #00c9ff, #92fe9d);-->
<!--  border: 3px solid white;-->
<!--  box-shadow: 0 3px 10px rgba(0, 0, 0, 0.3);-->
<!--}-->
<!--</style>-->
