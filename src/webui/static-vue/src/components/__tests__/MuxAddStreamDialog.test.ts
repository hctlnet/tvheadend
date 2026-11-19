// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Tvheadend contributors

/*
 * MuxAddStreamDialog — unit tests. The number of the input stream: what
 * it takes, what it emits, and that it opens empty.
 */
import { afterEach, describe, expect, it } from 'vitest'
import { enableAutoUnmount, mount } from '@vue/test-utils'
import PrimeVue from 'primevue/config'
import MuxAddStreamDialog from '../MuxAddStreamDialog.vue'
import { DIALOG_PASSTHROUGH_STUB } from './__helpers__/idnodeEditorTestUtils'

function mountDialog(visible = true) {
  return mount(MuxAddStreamDialog, {
    props: { visible, muxName: '12226V' },
    global: { plugins: [PrimeVue], stubs: { Dialog: DIALOG_PASSTHROUGH_STUB } },
  })
}

type Wrapper = ReturnType<typeof mountDialog>

const input = (w: Wrapper) => w.find('input.mux-add-stream__input')
const addButton = (w: Wrapper) => w.find('.mux-add-stream__add')

enableAutoUnmount(afterEach)

describe('MuxAddStreamDialog', () => {
  it('names the transponder and adds nothing while there is no number', () => {
    const w = mountDialog()
    expect(w.text()).toContain('12226V')
    expect(addButton(w).attributes('disabled')).toBeDefined()
  })

  it('emits the number, by the button and by Enter', async () => {
    const w = mountDialog()
    await input(w).setValue('14')
    await addButton(w).trigger('click')
    await input(w).setValue('0')
    await input(w).trigger('keydown', { key: 'Enter' })
    expect(w.emitted('add')).toEqual([[14], [0]])
  })

  it('takes 0 to 255 only', async () => {
    const w = mountDialog()
    for (const bad of ['256', '-1', '1.5']) {
      await input(w).setValue(bad)
      expect(addButton(w).attributes('disabled'), bad).toBeDefined()
      await input(w).trigger('keydown', { key: 'Enter' })
    }
    expect(w.emitted('add')).toBeUndefined()
    await input(w).setValue('255')
    expect(addButton(w).attributes('disabled')).toBeUndefined()
  })

  it('opens empty each time', async () => {
    const w = mountDialog()
    await input(w).setValue('14')
    await w.setProps({ visible: false })
    await w.setProps({ visible: true })
    expect((input(w).element as HTMLInputElement).value).toBe('')
  })

  it('closes by Cancel', async () => {
    const w = mountDialog()
    await w.findAll('button').find((b) => b.text() === 'Cancel')?.trigger('click')
    expect(w.emitted('close')).toHaveLength(1)
  })
})
