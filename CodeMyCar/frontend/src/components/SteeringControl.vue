<template>
  <div class="fit column items-center justify-center">
    <div class="row items-center" style="width: 100%; max-width: 300px">
      <q-slider
        v-model="steeringValue"
        :min="-45"
        :max="45"
        :step="1"
        label
        label-always
        color="light-green"
        track-color="light-blue-2"
        class="col"
        thumb-size="35px"
        @update:model-value="handleSteeringChange"
        name="steering"
      />
    </div>
  </div>
</template>

<script lang="ts">
export default {
  name: 'SteeringControl',
  props: {
    steeringAngle: {
      type: Number,
      default: 0,
    },
  },
  emits: ['steering-change'],
  data() {
    return {
      steeringValue: 0,
    }
  },
  watch: {
    steeringAngle: {
      handler(newVal) {
        this.steeringValue = newVal
      },
      immediate: true,
    },
  },
  methods: {
    handleSteeringChange(value) {
      this.$emit('steering-change', value)
    },
  },
}
</script>
