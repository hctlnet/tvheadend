<!--
  SPDX-License-Identifier: GPL-3.0-or-later
  Copyright (C) 2026 Tvheadend contributors
-->
<script setup lang="ts">
/*
 * MuxAddStreamDialog — asks for the number of an input stream (ISI) to
 * add to a DVB-S2 transponder.
 *
 * Opened by the mux tree (DvbMuxTreeView). It only asks for the number,
 * 0 to 255, and emits it; the caller makes the request
 * (`mpegts/mux/add_stream`) and shows what the server says.
 */
import { computed, ref, useId, watch } from 'vue'
import Button from 'primevue/button'
import Dialog from 'primevue/dialog'
import { useI18n } from '@/composables/useI18n'

const props = defineProps<{
  visible: boolean
  /* the name of the transponder, for the text */
  muxName: string
}>()

const emit = defineEmits<{
  close: []
  add: [stream: number]
}>()

const { t } = useI18n()

const labelId = useId()

/* an ISI is 8 bits (the MATYPE-2 byte of the baseband header) */
const MAX_STREAM = 255

const stream = ref<number | null>(null)

function isValid(n: number | null): n is number {
  return n !== null && Number.isInteger(n) && n >= 0 && n <= MAX_STREAM
}

const canAdd = computed(() => isValid(stream.value))

function onInput(ev: Event): void {
  const raw = (ev.target as HTMLInputElement).value
  stream.value = raw === '' ? null : Number(raw)
}

function onAdd(): void {
  if (isValid(stream.value)) emit('add', stream.value)
}

/* each time it opens: empty */
watch(
  () => props.visible,
  (open) => {
    if (open) stream.value = null
  },
)

/* the input gets the focus when the dialog shows */
const vFocus = { mounted: (el: HTMLElement) => el.focus() }
</script>

<template>
  <Dialog
    :visible="visible"
    :header="t('Add input stream')"
    modal
    :draggable="false"
    :style="{ width: '420px', maxWidth: 'calc(100vw - 32px)' }"
    @update:visible="(v) => { if (!v) emit('close') }"
  >
    <div class="mux-add-stream">
      <p class="mux-add-stream__note">
        {{ t('The transponder {0} carries several input streams. Each is a mux of its own below it.', muxName) }}
      </p>
      <div class="mux-add-stream__field">
        <label :for="labelId" class="mux-add-stream__label">{{ t('Input stream (ISI)') }}</label>
        <input
          :id="labelId"
          v-focus
          class="mux-add-stream__input"
          type="number"
          min="0"
          :max="MAX_STREAM"
          step="1"
          inputmode="numeric"
          placeholder="0–255"
          :value="stream ?? ''"
          @input="onInput"
          @keydown.enter.prevent="onAdd"
        />
      </div>
    </div>
    <template #footer>
      <Button severity="secondary" outlined size="small" @click="emit('close')">
        {{ t('Cancel') }}
      </Button>
      <Button size="small" class="mux-add-stream__add" :disabled="!canAdd" @click="onAdd">
        {{ t('Add') }}
      </Button>
    </template>
  </Dialog>
</template>

<style scoped>
.mux-add-stream {
  display: flex;
  flex-direction: column;
  gap: var(--tvh-space-3);
  padding: var(--tvh-space-2) 0;
}

.mux-add-stream__note {
  margin: 0;
  color: var(--tvh-text-muted);
  font-size: var(--tvh-text-md);
}

.mux-add-stream__field {
  display: flex;
  align-items: center;
  gap: var(--tvh-space-3);
}

.mux-add-stream__label {
  font-size: var(--tvh-text-md);
}

.mux-add-stream__input {
  width: 7rem;
  min-height: 36px;
  padding: 6px 10px;
  background: var(--tvh-bg-page);
  color: var(--tvh-text);
  border: 1px solid var(--tvh-border);
  border-radius: var(--tvh-radius-sm);
  font: inherit;
  font-size: var(--tvh-text-md);
}

.mux-add-stream__input:focus {
  outline: 2px solid var(--tvh-primary);
  outline-offset: 1px;
}
</style>
