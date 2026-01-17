<template>
  <q-layout view="lHh Lpr lFf">
    <q-page-container>
      <RemoteControl />
    </q-page-container>
  </q-layout>
</template>

<script lang="ts">
import RemoteControl from './views/RemoteControl.vue'

export default {
  name: 'App',
  components: {
    RemoteControl
  }
}
</script>

<style>
body {
  margin: 0;
  padding: 0;
  font-family: 'Segoe UI', Roboto, 'Helvetica Neue', Arial, sans-serif;
  background: linear-gradient(135deg, #1a2a6c, #2c3e50);
  height: 100vh;
  overflow: hidden;
  color: white;
  user-select: none;
  touch-action: manipulation;
}
</style>
