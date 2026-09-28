<script setup>
import { ref } from 'vue'
import Checkbox from '../../../Checkbox.vue'

const props = defineProps({
  config: Object,
})

const config = ref(props.config)
</script>

<template>
  <div id="pyrowave-encoder" class="config-page">
    <Checkbox class="mb-3"
              id="pyrowave_enabled"
              locale-prefix="config"
              v-model="config.pyrowave_enabled"
              default="disabled"
    ></Checkbox>

    <div class="mb-3">
      <label for="pyrowave_bitrate" class="form-label">{{ $t('config.pyrowave_bitrate') }}</label>
      <input type="number" class="form-control" id="pyrowave_bitrate"
             min="0" max="2000000" step="1000" v-model="config.pyrowave_bitrate" />
      <div class="form-text">{{ $t('config.pyrowave_bitrate_desc') }}</div>
    </div>
  </div>
</template>
