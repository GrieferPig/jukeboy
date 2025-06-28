<script setup lang="ts">
import { ref } from "vue";

const esp32BaseUrl = "http://thejuke.box"; // ESP32 AP IP Address
const apiMessage = ref("");

const jumpSeconds = ref(30);
const ffSeconds = ref(10);
const rewSeconds = ref(10);
const gainValue = ref(0.1); // Default gain, will be clamped 0.0 to 0.2 by ESP32
const playbackMode = ref<'loop' | 'shuffle'>('loop');

async function callApi(endpoint: string, params?: Record<string, any>) {
  let url = `${esp32BaseUrl}${endpoint}`;
  if (params) {
    const queryParams = new URLSearchParams(params);
    url += `?${queryParams.toString()}`;
  }
  try {
    apiMessage.value = "Sending command...";
    const response = await fetch(url);
    const text = await response.text();
    apiMessage.value = `Response: ${text}`;
  } catch (error) {
    apiMessage.value = `Error: ${error}`;
    console.error("API call failed:", error);
  }
}

function playNext() {
  callApi("/audio/play_next");
}

function playPrevious() {
  callApi("/audio/play_previous");
}

function restartTrack() {
  callApi("/audio/restart");
}

function togglePause() {
  callApi("/audio/toggle_pause");
}

function getCurrentTrackInfo() {
  callApi("/audio/info");
}

function getPlaybackStatus() {
  callApi("/audio/status");
}

function jumpToPosition() {
  callApi("/audio/jump", { seconds: jumpSeconds.value });
}

function fastForward() {
  callApi("/audio/ffwd", { seconds: ffSeconds.value });
}

function rewind() {
  callApi("/audio/rewind", { seconds: rewSeconds.value });
}

function setGain() {
  // Clamp gain on client-side as well for immediate feedback, though ESP32 also clamps
  const clampedGain = Math.max(0.0, Math.min(gainValue.value, 0.2));
  gainValue.value = clampedGain; // Update the ref if it was clamped
  callApi("/audio/set_gain", { gain: clampedGain });
}

async function togglePlaybackMode() {
  const newMode = playbackMode.value === 'loop' ? 'shuffle' : 'loop';
  await callApi("/audio/set_mode", { mode: newMode });
  // Optimistically update UI if command likely succeeded
  if (!apiMessage.value.toLowerCase().includes("error") && 
      !apiMessage.value.toLowerCase().includes("missing") &&
      apiMessage.value.toLowerCase().includes("command sent")) {
    playbackMode.value = newMode;
  }
}
</script>

<template>
  <header>
    <h1>the-jukebox Audio Control</h1>
  </header>

  <main>
    <div>
      <h2>Playback Controls</h2>
      <button @click="playPrevious">Previous</button>
      <button @click="togglePause">Toggle Pause/Play</button>
      <button @click="playNext">Next</button>
      <button @click="restartTrack">Restart Track</button>
    </div>

    <div>
      <h2>Seeking</h2>
      <div>
        <label for="jump">Jump to (seconds): </label>
        <input type="number" id="jump" v-model.number="jumpSeconds" />
        <button @click="jumpToPosition">Jump</button>
      </div>
      <div>
        <label for="ffwd">Fast Forward (seconds): </label>
        <input type="number" id="ffwd" v-model.number="ffSeconds" />
        <button @click="fastForward">FFWD</button>
      </div>
      <div>
        <label for="rewind">Rewind (seconds): </label>
        <input type="number" id="rewind" v-model.number="rewSeconds" />
        <button @click="rewind">Rewind</button>
      </div>
    </div>

    <div>
      <h2>Volume</h2>
      <label for="gain">Gain (0.0 - 0.2): </label>
      <input
        type="number"
        id="gain"
        v-model.number="gainValue"
        step="0.01"
        min="0.0"
        max="0.2"
      />
      <button @click="setGain">Set Gain</button>
    </div>

    <div>
      <h2>Playback Mode</h2>
      <button @click="togglePlaybackMode">
        Toggle Mode (Current: {{ playbackMode === 'loop' ? 'Loop Playlist' : 'Shuffle Playlist' }})
      </button>
    </div>

    <div>
      <h2>Information</h2>
      <button @click="getCurrentTrackInfo">Get Track Info</button>
      <button @click="getPlaybackStatus">Get Playback Status</button>
    </div>

    <div
      v-if="apiMessage"
      style="margin-top: 20px; padding: 10px; border: 1px solid #ccc"
    >
      <p>{{ apiMessage }}</p>
    </div>
  </main>
</template>

<style scoped>
/* No custom styling as per request, using browser defaults for basic controls */
main > div {
  margin-bottom: 20px;
}
button {
  margin: 5px;
}
input[type="number"] {
  width: 60px;
  margin-right: 5px;
}
label {
  margin-right: 5px;
}
</style>
