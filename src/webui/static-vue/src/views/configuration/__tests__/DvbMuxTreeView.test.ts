// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Tvheadend contributors

/*
 * DvbMuxTreeView — the Muxes tab as a tree. The API answers come from
 * a small fake server below: it keeps networks, muxes and services and
 * applies the grid filters the way tvheadend does (a string filter is
 * a case-insensitive extended regular expression, `api_idnode.c`).
 */
import { describe, expect, it, vi } from 'vitest'
import { flushPromises, mount } from '@vue/test-utils'
import PrimeVue from 'primevue/config'
import { fireComet } from '@/test/__helpers__/cometClientMock'

const stubs = await vi.hoisted(() => import('@/test/__helpers__/gridViewTestUtils'))
const spies = vi.hoisted(() => ({ helpToggle: vi.fn(() => Promise.resolve()), openPlay: vi.fn() }))
vi.mock('@/api/client', () => stubs.apiClient)
vi.mock('@/api/comet', () => import('@/test/__helpers__/cometClientMock'))
vi.mock('@/components/IdnodeEditor.vue', () => stubs.idnodeEditor)
vi.mock('@/components/IdnodePickEntityDialog.vue', () => stubs.emptyComponent)
/* the dialog that asks for the number: a button that answers 5 */
vi.mock('@/components/MuxAddStreamDialog.vue', async () => {
  const { defineComponent, h } = await import('vue')
  return {
    default: defineComponent({
      name: 'MuxAddStreamDialog',
      props: { visible: Boolean, muxName: { type: String, default: '' } },
      emits: ['add', 'close'],
      setup(props, { emit }) {
        return () =>
          props.visible
            ? h('button', { class: 'add-stream-stub', onClick: () => emit('add', 5) }, props.muxName)
            : null
      },
    }),
  }
})
vi.mock('vue-router', () => stubs.vueRouter)
vi.mock('@/composables/useConfirmDialog', () => stubs.confirmDialog)
vi.mock('@/composables/useToastNotify', () => stubs.toastNotify)
vi.mock('@/composables/useHelp', async () => {
  const { ref } = await import('vue')
  return { useHelp: () => ({ isOpen: ref(false), toggle: spies.helpToggle }) }
})
vi.mock('@/utils/playUrl', () => ({ openPlay: spies.openPlay }))

import DvbMuxTreeView from '../DvbMuxTreeView.vue'

stubs.resetEachTest()

type Row = Record<string, unknown>

interface FakeServer {
  networks: Row[]
  muxes: Row[]
  services: Row[]
  /* when set: an answer waits for the promise it returns */
  gate?: (ep: string) => Promise<void> | undefined
  /* the answers of `mpegts/mux/add_stream`, one per request */
  addStream?: Row[]
}

/* a DVB-S network with one transponder and its service, an IPTV
 * network with one mux */
function server(): FakeServer {
  return {
    networks: [
      { uuid: 'n1', class: 'dvb_network_dvbs', networkname: 'Astra', num_mux: 1, num_svc: 1, num_chn: 1 },
      {
        uuid: 'n2', class: 'iptv_auto_network', networkname: 'IPTV Automatic Network',
        num_mux: 1, num_svc: 0, num_chn: 0,
      },
    ],
    muxes: [
      {
        uuid: 'm1', name: '11494H', network: 'Astra', network_uuid: 'n1', tstype: 0, delsys: 'DVB-S2',
        modulation: 'PSK/8', fec: '2/3', symbolrate: 22000000, pnetwork_name: 'ASTRA 1',
        frequency: 11494000, polarisation: 'H', num_svc: 1, num_chn: 1, scan_state: 0, scan_result: 1, enabled: 1,
      },
      {
        uuid: 'i1', name: 'Channel One', network: 'IPTV Automatic Network', network_uuid: 'n2', tstype: 0,
        num_svc: 0, num_chn: 0, scan_state: 0, scan_result: 0, enabled: 1,
      },
    ],
    services: [
      {
        uuid: 's1', svcname: 'Das Erste HD', sid: 10301, network: 'Astra', multiplex_uuid: 'm1',
        dvb_servicetype: 0x19, enabled: true, channel: ['c1'],
      },
    ],
  }
}

/* a multistream transponder with two input streams in network n1 */
function multistream(s: FakeServer) {
  s.muxes.push(
    {
      uuid: 't1', name: '12226V', network: 'Astra', network_uuid: 'n1', tstype: 0, delsys: 'DVB-S2',
      multistream: true, num_svc: 0, num_chn: 0, scan_state: 0, scan_result: 0, enabled: 1,
    },
    {
      uuid: 't1b', name: '12226V ISI 14', network: 'Astra', network_uuid: 'n1', tstype: 7, parent_mux: 't1',
      stream_id: 14, delsys: 'DVB-S2', num_svc: 19, num_chn: 2, scan_state: 0, scan_result: 1, enabled: 1,
    },
    {
      uuid: 't1a', name: '12226V ISI 3', network: 'Astra', network_uuid: 'n1', tstype: 7, parent_mux: 't1',
      stream_id: 3, delsys: 'DVB-S2', num_svc: 12, num_chn: 1, scan_state: 0, scan_result: 2, enabled: 1,
    },
  )
  s.networks[0].num_mux = 4
}

/* the grid filters as the server applies them */
function filtered(rows: Row[], params: Row): Row[] {
  const filter = (params.filter ?? []) as Array<{ field: string; type: string; value: string }>
  return rows.filter((r) =>
    filter.every((f) => {
      if (f.type !== 'string') return true
      let re: RegExp
      try {
        re = new RegExp(f.value, 'i')
      } catch {
        return true /* the server drops a filter that does not compile */
      }
      return re.test(String(r[f.field] ?? ''))
    }),
  )
}

function serve(s: FakeServer) {
  stubs.apiMock.mockImplementation(async (ep: string, params: Row = {}) => {
    const wait = s.gate?.(ep)
    if (wait) await wait
    if (ep === 'mpegts/network/grid') return { entries: s.networks, total: s.networks.length }
    if (ep === 'mpegts/mux/grid' || ep === 'mpegts/service/grid') {
      const rows = filtered(ep === 'mpegts/mux/grid' ? s.muxes : s.services, params)
      const limit = typeof params.limit === 'number' ? params.limit : rows.length
      return { entries: rows.slice(0, limit), total: rows.length }
    }
    if (ep === 'mpegts/mux/add_stream') return s.addStream?.shift() ?? {}
    return {}
  })
}

function calls(ep: string): Row[] {
  return stubs.apiMock.mock.calls.filter((c) => c[0] === ep).map((c) => (c[1] ?? {}) as Row)
}

async function mountTree(s: FakeServer = server()) {
  serve(s)
  const w = mount(DvbMuxTreeView, { global: { plugins: [PrimeVue] } })
  await flushPromises()
  return w
}

type Wrapper = Awaited<ReturnType<typeof mountTree>>

/* the text of the rows shown, one string per row */
function rowTexts(w: Wrapper): string[] {
  return w.findAll('tbody tr').map((tr) => tr.text().replace(/\s+/g, ' ').trim())
}

function shows(w: Wrapper, text: string): boolean {
  return rowTexts(w).some((r) => r.includes(text))
}

function rowOf(w: Wrapper, text: string) {
  const row = w.findAll('tbody tr').find((tr) => tr.text().includes(text))
  if (!row) throw new Error(`no row with "${text}": ${rowTexts(w).join(' | ')}`)
  return row
}

/* click the expand toggle of the row that shows `text` */
async function expand(w: Wrapper, text: string) {
  await rowOf(w, text).find('button.mux-tree__toggle').trigger('click')
  await flushPromises()
}

function button(w: Wrapper, label: string) {
  const b = w.findAll('button').find((x) => x.text().includes(label))
  if (!b) throw new Error(`no button "${label}"`)
  return b
}

describe('DvbMuxTreeView networks', () => {
  it('lists the IPTV networks too, their muxes as a flat list', async () => {
    const w = await mountTree()
    expect(shows(w, 'Astra')).toBe(true)
    expect(shows(w, 'IPTV Automatic Network')).toBe(true)
    await expand(w, 'IPTV Automatic Network')
    expect(shows(w, 'Channel One')).toBe(true)
  })

  it('asks for the muxes of a network by its uuid, and only for the fields it shows', async () => {
    const w = await mountTree()
    await expand(w, 'Astra (1)')
    expect(shows(w, '11494H')).toBe(true)
    const call = calls('mpegts/mux/grid')[0]
    expect(call.filter).toEqual([{ type: 'string', field: 'network_uuid', value: '^n1$' }])
    expect(String(call.list).split(',')).toEqual(expect.arrayContaining(['name', 'tstype', 'parent_mux', 'stream_id']))
  })

  it('switches to the list view of the tab', async () => {
    const w = await mountTree()
    await button(w, 'List view').trigger('click')
    expect(stubs.router.push).toHaveBeenCalledWith({ name: 'config-dvb-mux-list' })
  })

  it('says how to get muxes when there are none', async () => {
    const s = server()
    s.networks = []
    const w = await mountTree(s)
    expect(w.find('.mux-tree__empty').text()).toContain('No muxes defined')
  })
})

describe('DvbMuxTreeView sub-muxes', () => {
  it('shows the input streams of a multistream transponder below it, by their number', async () => {
    const s = server()
    multistream(s)
    const w = await mountTree(s)
    await expand(w, 'Astra (4)')
    const texts = rowTexts(w)
    const i = texts.findIndex((r) => r.startsWith('12226V'))
    expect(texts[i]).toContain('multistream')
    expect(texts[i + 1]).toMatch(/^ISI 3\s*TS \(ISI\)/)
    expect(texts[i + 2]).toMatch(/^ISI 14\s*TS \(ISI\)/)
    /* and closes them */
    await expand(w, '12226V')
    expect(shows(w, 'ISI 3')).toBe(false)
  })

  it('marks a sub-mux whose transponder is gone', async () => {
    const s = server()
    s.muxes.push({
      uuid: 'o1', name: '10744H ISI 2', network: 'Astra', network_uuid: 'n1', tstype: 7, parent_mux: 'gone',
      stream_id: 2, num_svc: 0, num_chn: 0, scan_state: 0, enabled: 1,
    })
    const w = await mountTree(s)
    await expand(w, 'Astra (1)')
    expect(rowOf(w, '10744H ISI 2').text()).toContain('transponder missing')
  })

  it('opens and closes all', async () => {
    const s = server()
    multistream(s)
    const w = await mountTree(s)
    await button(w, 'Expand all').trigger('click')
    await flushPromises()
    expect(shows(w, 'Channel One')).toBe(true)
    expect(shows(w, 'ISI 14')).toBe(true)
    expect(w.find('.mux-tree__count').text()).toContain('2 networks, 5 muxes loaded')
    await button(w, 'Collapse all').trigger('click')
    expect(rowTexts(w)).toHaveLength(2)
  })
})

describe('DvbMuxTreeView columns', () => {
  it('shows the tuning and the provider network of a transponder in its row', async () => {
    const w = await mountTree()
    await expand(w, 'Astra (1)')
    const row = rowOf(w, '11494H')
    expect(row.find('.mux-tree__tuning').text()).toBe('DVB-S2 · 8PSK 2/3 · SR 22000')
    expect(row.find('.mux-tree__provider').text()).toBe('ASTRA 1')
    expect(row.text()).toContain('OK')
    expect(String(calls('mpegts/mux/grid')[0].list).split(',')).toEqual(
      expect.arrayContaining(['delsys', 'symbolrate', 'scan_state', 'scan_result', 'num_svc', 'enabled']),
    )
  })

  it('adds the services and channels of the input streams to those of their transponder', async () => {
    const s = server()
    multistream(s)
    const w = await mountTree(s)
    await expand(w, 'Astra (4)')
    const cells = rowOf(w, '12226V').findAll('td.mux-tree__num')
    expect(cells.map((c) => c.text())).toEqual(['31', '3'])
    expect(cells[0].attributes('title')).toBe('0 own + 31 in sub-muxes')
    /* one of the two input streams scanned with success */
    expect(rowOf(w, '12226V').text()).toContain('1/2')
    expect(rowOf(w, 'ISI 3').text()).toContain('FAIL')
  })

  it('says what is queued, scanning, in the idle scan queue or not enabled', async () => {
    const s = server()
    multistream(s)
    s.muxes[0].scan_state = 2
    s.muxes[2].scan_state = 1
    s.muxes[3].scan_state = 3
    s.muxes[4].enabled = 0
    const w = await mountTree(s)
    await expand(w, 'Astra (4)')
    expect(rowOf(w, '11494H').text()).toContain('idle queue')
    expect(rowOf(w, '12226V').text()).toContain('queued')
    expect(rowOf(w, 'ISI 14').text()).toContain('scanning…')
    expect(rowOf(w, 'ISI 3').text()).toContain('disabled')
    expect(rowOf(w, 'ISI 3').classes()).toContain('mux-tree__row--off')
  })
})

/* rename network n1 (in its mux and service rows too) */
function renameAstra(s: FakeServer, name: string | undefined) {
  if (name === undefined) delete s.networks[0].networkname
  else s.networks[0].networkname = name
  const shown = name || 'unknown'
  for (const m of s.muxes) if (m.network_uuid === 'n1') m.network = shown
  for (const x of s.services) if (x.multiplex_uuid === 'm1') x.network = shown
}

describe('DvbMuxTreeView services', () => {
  it('shows the services of a mux below it, with type, SID and streams', async () => {
    const s = server()
    s.services[0].streams = 'H.264 1080i, AC-3 deu'
    s.services[0].encrypted = true
    s.services[0].caid = '1830:000000,1843:000000'
    const w = await mountTree(s)
    await expand(w, 'Astra (1)')
    await expand(w, '11494H')
    const row = rowOf(w, 'Das Erste HD')
    expect(row.text()).toContain('HD TV')
    expect(row.find('.mux-tree__sid').text()).toBe('10301 · 1830, 1843')
    expect(row.find('.mux-tree__streams').text()).toBe('H.264 1080i, AC-3 deu')
    const svcList = String(calls('mpegts/service/grid')[0].list).split(',')
    expect(svcList).toEqual(expect.arrayContaining(['svcname', 'sid', 'multiplex_uuid', 'network', 'streams']))
    expect(svcList).not.toContain('source')
  })

  it('puts the services of a mux with sub-muxes into a row of their own', async () => {
    const s = server()
    multistream(s)
    s.muxes[2].num_svc = 1
    s.services.push({ uuid: 's2', svcname: 'Stray', sid: 7, network: 'Astra', multiplex_uuid: 't1', enabled: true })
    const w = await mountTree(s)
    await expand(w, 'Astra (4)')
    expect(shows(w, 'Stray')).toBe(false)
    await expand(w, 'Services (1)')
    expect(shows(w, 'Stray')).toBe(true)
  })

  it('finds the services of a network with ( ) in its name', async () => {
    const s = server()
    renameAstra(s, 'Astra 1 (19.2E)')
    const w = await mountTree(s)
    await expand(w, 'Astra 1 (19.2E)')
    await expand(w, '11494H')
    expect(shows(w, 'Das Erste HD')).toBe(true)
    expect(calls('mpegts/service/grid')[0].filter).toEqual([
      { type: 'string', field: 'network', value: '^Astra 1 \\(19\\.2E\\)$' },
    ])
  })

  it('loads a network without a name, its services by the name the server gives it', async () => {
    const s = server()
    renameAstra(s, undefined)
    const w = await mountTree(s)
    await expand(w, 'n1')
    await expand(w, '11494H')
    expect(shows(w, 'Das Erste HD')).toBe(true)
    expect(calls('mpegts/service/grid')[0].filter).toEqual([{ type: 'string', field: 'network', value: '^unknown$' }])
  })

  it('keeps out the services of another network the name filter finds', async () => {
    const s = server()
    s.networks.push({ uuid: 'n3', networkname: 'ASTRA', num_mux: 1, num_svc: 1, num_chn: 0 })
    s.muxes.push({
      uuid: 'm3', name: '10744H', network: 'ASTRA', network_uuid: 'n3', tstype: 0,
      num_svc: 1, num_chn: 0, scan_state: 0, enabled: 1,
    })
    s.services.push({ uuid: 's3', svcname: 'Other', sid: 1, network: 'ASTRA', multiplex_uuid: 'm3', enabled: true })
    s.muxes[0].num_svc = 2
    const w = await mountTree(s)
    await expand(w, 'ASTRA (1)')
    await expand(w, 'Astra (1)')
    await expand(w, '11494H')
    expect(shows(w, 'Das Erste HD')).toBe(true)
    expect(shows(w, 'Other')).toBe(false)
  })
})

/* the answers of the mux grid wait until released, one by one */
function holdMuxGrid(s: FakeServer) {
  const held: Array<() => void> = []
  s.gate = (ep) => (ep === 'mpegts/mux/grid' ? new Promise<void>((r) => held.push(r)) : undefined)
  return {
    release: () => held.shift()?.(),
    stop: () => {
      s.gate = undefined
      while (held.length) held.shift()?.()
    },
  }
}

describe('DvbMuxTreeView Reload', () => {
  it('keeps the open rows and loads their muxes again', async () => {
    const s = server()
    const w = await mountTree(s)
    await expand(w, 'Astra (1)')
    const before = calls('mpegts/mux/grid').length
    s.muxes[0].name = '11495H'
    await button(w, 'Reload').trigger('click')
    await flushPromises()
    expect(calls('mpegts/mux/grid')).toHaveLength(before + 1)
    expect(shows(w, '11495H')).toBe(true)
  })

  it('loads a network once at a time, the later state wins', async () => {
    const s = server()
    const w = await mountTree(s)
    await expand(w, 'Astra (1)')
    const before = calls('mpegts/mux/grid').length
    const gate = holdMuxGrid(s)
    await button(w, 'Reload').trigger('click')
    await flushPromises()
    await button(w, 'Reload').trigger('click')
    await flushPromises()
    /* the second load waits for the first */
    expect(calls('mpegts/mux/grid')).toHaveLength(before + 1)
    s.muxes[0].name = '11495H'
    gate.release()
    await flushPromises()
    expect(calls('mpegts/mux/grid')).toHaveLength(before + 2)
    gate.stop()
    await flushPromises()
    expect(shows(w, '11495H')).toBe(true)
  })

  it('keeps the services of an open mux and loads them again', async () => {
    const w = await mountTree()
    await expand(w, 'Astra (1)')
    await expand(w, '11494H')
    expect(shows(w, 'Das Erste HD')).toBe(true)
    const before = calls('mpegts/service/grid').length
    await button(w, 'Reload').trigger('click')
    await flushPromises()
    expect(calls('mpegts/service/grid')).toHaveLength(before + 1)
    expect(shows(w, 'Das Erste HD')).toBe(true)
    expect(w.text()).not.toContain('Loading…')
  })

  it('loads the services again that failed to load', async () => {
    const s = server()
    const w = await mountTree(s)
    await expand(w, 'Astra (1)')
    s.gate = (ep) => (ep === 'mpegts/service/grid' ? Promise.reject(new Error('busy')) : undefined)
    await expand(w, '11494H')
    expect(w.text()).toContain('Loading…')
    s.gate = undefined
    await button(w, 'Reload').trigger('click')
    await flushPromises()
    expect(shows(w, 'Das Erste HD')).toBe(true)
  })
})

describe('DvbMuxTreeView load errors', () => {
  it('shows a failed load until that load succeeds', async () => {
    const s = server()
    const w = await mountTree(s)
    s.gate = (ep) => (ep === 'mpegts/mux/grid' ? Promise.reject(new Error('busy')) : undefined)
    await expand(w, 'Astra (1)')
    expect(w.find('.mux-tree__error').text()).toBe('Failed to load: busy')
    /* the networks load fine: the error of the muxes stays */
    vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout'] })
    try {
      fireComet('mpegts_network', { change: ['n1'] })
      await vi.advanceTimersByTimeAsync(1500)
    } finally {
      vi.useRealTimers()
    }
    await flushPromises()
    expect(w.find('.mux-tree__error').exists()).toBe(true)
    s.gate = undefined
    await button(w, 'Reload').trigger('click')
    await flushPromises()
    expect(w.find('.mux-tree__error').exists()).toBe(false)
    expect(shows(w, '11494H')).toBe(true)
  })
})

async function typeFilter(w: Wrapper, text: string) {
  await w.find('.mux-tree__search input').setValue(text)
  /* the filter waits 250 ms for the typing to end */
  await new Promise((r) => setTimeout(r, 300))
  await flushPromises()
}

describe('DvbMuxTreeView filter', () => {
  it('finds a mux in a network that was not open, and shows only what matches', async () => {
    const s = server()
    multistream(s)
    const w = await mountTree(s)
    await typeFilter(w, 'isi 14')
    expect(shows(w, 'ISI 14')).toBe(true)
    expect(shows(w, '12226V')).toBe(true)
    expect(shows(w, 'ISI 3')).toBe(false)
    expect(shows(w, '11494H')).toBe(false)
    expect(shows(w, 'IPTV Automatic Network')).toBe(false)
  })

  it('closes a row the filter opened', async () => {
    const w = await mountTree()
    await typeFilter(w, '11494')
    expect(shows(w, '11494H')).toBe(true)
    await expand(w, 'Astra (1)')
    expect(shows(w, '11494H')).toBe(false)
    await expand(w, 'Astra (1)')
    expect(shows(w, '11494H')).toBe(true)
  })

  it('shows the muxes whose scan failed, or the disabled ones', async () => {
    const s = server()
    multistream(s)
    s.muxes[1].enabled = 0
    const w = await mountTree(s)
    await w.find('select.mux-tree__select').setValue('failed')
    await flushPromises()
    expect(shows(w, 'ISI 3')).toBe(true)
    expect(shows(w, 'ISI 14')).toBe(false)
    expect(shows(w, 'Channel One')).toBe(false)
    await w.find('select.mux-tree__select').setValue('disabled')
    await flushPromises()
    expect(shows(w, 'Channel One')).toBe(true)
    expect(shows(w, 'ISI 3')).toBe(false)
  })

  it('shows only the transponders with sub-muxes', async () => {
    const s = server()
    multistream(s)
    const w = await mountTree(s)
    await expand(w, 'Astra (4)')
    expect(shows(w, '11494H')).toBe(true)
    const boxes = w.findAll('label.mux-tree__check input')
    await boxes[boxes.length - 1].setValue(true)
    expect(shows(w, '11494H')).toBe(false)
    expect(shows(w, '12226V')).toBe(true)
  })
})

/* n more transponders in network n1, one service each */
function manyMuxes(s: FakeServer, n: number) {
  for (let j = 0; j < n; j++) {
    s.muxes.push({
      uuid: `x${j}`, name: `${10700 + j}V`, network: 'Astra', network_uuid: 'n1', tstype: 0,
      num_svc: 1, num_chn: 0, scan_state: 0, scan_result: 1, enabled: 1,
    })
    s.services.push({ uuid: `y${j}`, svcname: `Radio ${j}`, sid: j + 1, network: 'Astra', multiplex_uuid: `x${j}` })
  }
  s.networks[0].num_mux = n + 1
}

async function searchServiceNames(w: Wrapper, text: string) {
  await w.find('label.mux-tree__check input').setValue(true)
  await typeFilter(w, text)
}

describe('DvbMuxTreeView search in the service names', () => {
  it('searches the service names on the server, once, and shows the first matches', async () => {
    const s = server()
    manyMuxes(s, 600)
    const w = await mountTree(s)
    await searchServiceNames(w, 'radio')
    const search = calls('mpegts/service/grid').filter((c) => JSON.stringify(c.filter).includes('svcname'))
    expect(search).toHaveLength(1)
    expect(search[0].filter).toEqual([{ type: 'string', field: 'svcname', value: 'radio' }])
    expect(search[0].limit).toBe(500)
    /* no services of whole networks for it */
    expect(calls('mpegts/service/grid')).toHaveLength(1)
    expect(w.find('.mux-tree__hits').text()).toContain('500')
    expect(w.find('.mux-tree__hits').text()).toContain('600')
    expect(shows(w, 'Radio 0')).toBe(true)
  })

  it('escapes the search text for the server', async () => {
    const s = server()
    s.services.push({ uuid: 's9', svcname: 'ORF 2 (W)', sid: 9, network: 'Astra', multiplex_uuid: 'm1' })
    s.muxes[0].num_svc = 2
    const w = await mountTree(s)
    await searchServiceNames(w, '2 (w')
    expect(shows(w, 'ORF 2 (W)')).toBe(true)
    expect(shows(w, 'Das Erste HD')).toBe(false)
  })
})

/* the editor the view rendered: what it was opened with */
function editor(w: Wrapper) {
  const c = w.findComponent({ name: 'IdnodeEditor' })
  return { attrs: c.vm.$attrs as Record<string, unknown>, emit: (ev: string, ...a: unknown[]) => c.vm.$emit(ev, ...a) }
}

describe('DvbMuxTreeView editor', () => {
  it('opens the editor of a mux, a service and a network by a double click', async () => {
    const w = await mountTree()
    await expand(w, 'Astra (1)')
    await expand(w, '11494H')
    await rowOf(w, '11494H').trigger('dblclick')
    expect(stubs.captured.editor.uuid).toBe('m1')
    expect(editor(w).attrs.title).toBe('Edit Mux')
    await rowOf(w, 'Das Erste HD').trigger('dblclick')
    expect(stubs.captured.editor.uuid).toBe('s1')
    await rowOf(w, 'Astra (1)').trigger('dblclick')
    expect(stubs.captured.editor.uuid).toBe('n1')
  })

  it('loads the network of an edited mux again when the editor closes', async () => {
    const w = await mountTree()
    await expand(w, 'Astra (1)')
    await rowOf(w, '11494H').trigger('dblclick')
    const before = calls('mpegts/mux/grid').length
    editor(w).emit('close')
    await flushPromises()
    expect(stubs.captured.editor.uuid).toBeNull()
    expect(calls('mpegts/mux/grid')).toHaveLength(before + 1)
  })

  it('adds a mux to the network of the row clicked last, and goes on with its editor', async () => {
    const w = await mountTree()
    await rowOf(w, 'Astra (1)').trigger('click')
    await button(w, 'Add').trigger('click')
    expect(editor(w).attrs['create-base']).toBe('mpegts/network')
    expect(editor(w).attrs['parent-scoped']).toMatchObject({
      createEndpoint: 'mpegts/network/mux_create',
      params: { uuid: 'n1' },
    })
    editor(w).emit('created', 'm9')
    await flushPromises()
    expect(stubs.captured.editor.uuid).toBe('m9')
    expect(editor(w).attrs['create-base']).toBeNull()
  })

  it('asks for the network when no row was clicked', async () => {
    const w = await mountTree()
    const picker = w.findComponent({ name: 'EmptyStub' })
    expect(picker.vm.$attrs.visible).toBe(false)
    await button(w, 'Add').trigger('click')
    expect(picker.vm.$attrs.visible).toBe(true)
    picker.vm.$emit('pick', 'n2', 'IPTV Automatic Network')
    await flushPromises()
    expect(editor(w).attrs['parent-scoped']).toMatchObject({ params: { uuid: 'n2' } })
  })
})

describe('DvbMuxTreeView selection', () => {
  const pick = (w: Wrapper, text: string, opts: Record<string, boolean> = {}) =>
    rowOf(w, text).trigger('click', opts)
  const selectedTexts = (w: Wrapper) =>
    w.findAll('tr.mux-tree__row--selected').map((tr) => tr.text().replace(/\s+/g, ' ').trim())

  async function mountBothOpen(s: FakeServer = server()) {
    const w = await mountTree(s)
    await expand(w, 'Astra (1)')
    await expand(w, 'IPTV Automatic Network')
    return w
  }

  it('selects one row by a click, more with Ctrl, all shown with the box in the header', async () => {
    const w = await mountBothOpen()
    await pick(w, '11494H')
    await pick(w, 'Channel One')
    expect(selectedTexts(w)).toHaveLength(1)
    await pick(w, '11494H', { ctrlKey: true })
    expect(selectedTexts(w)).toHaveLength(2)
    expect(w.find('.mux-tree__count').text()).toContain('2 selected')
    await w.find('thead input[type="checkbox"]').trigger('change')
    expect(selectedTexts(w)).toHaveLength(0)
  })

  it('selects a Shift range from the row clicked, after rows above it opened', async () => {
    const w = await mountTree()
    await expand(w, 'IPTV Automatic Network')
    await pick(w, 'Channel One')
    /* opening Astra moves the rows below it */
    await expand(w, 'Astra (1)')
    await pick(w, '11494H', { shiftKey: true })
    const sel = selectedTexts(w)
    expect(sel.some((r) => r.includes('Channel One'))).toBe(true)
    expect(sel.some((r) => r.includes('11494H'))).toBe(true)
  })

  it('edits several of a kind at once, not a mux and a service together', async () => {
    const w = await mountBothOpen()
    await expand(w, '11494H')
    await pick(w, '11494H')
    await pick(w, 'Channel One', { ctrlKey: true })
    await button(w, 'Edit').trigger('click')
    expect(stubs.captured.editor.uuids).toEqual(['m1', 'i1'])
    await pick(w, 'Das Erste HD', { ctrlKey: true })
    expect(button(w, 'Edit').attributes('disabled')).toBeDefined()
  })

  it('deletes the selection and loads its networks again', async () => {
    const w = await mountBothOpen()
    await pick(w, '11494H')
    const before = calls('mpegts/mux/grid').length
    await button(w, 'Delete').trigger('click')
    await flushPromises()
    expect(calls('idnode/delete')).toEqual([{ uuid: '["m1"]' }])
    expect(calls('mpegts/mux/grid')).toHaveLength(before + 1)
    expect(selectedTexts(w)).toHaveLength(0)
  })

  it('says when a delete fails', async () => {
    const s = server()
    const w = await mountBothOpen(s)
    await pick(w, '11494H')
    s.gate = (ep) => (ep === 'idnode/delete' ? Promise.reject(new Error('in use')) : undefined)
    await button(w, 'Delete').trigger('click')
    await flushPromises()
    expect(stubs.toast.error).toHaveBeenCalledWith('Failed to delete: in use')
  })
})

/* the saves fail */
function failSaves(s: FakeServer) {
  s.gate = (ep) => (ep === 'idnode/save' ? Promise.reject(new Error('session expired')) : undefined)
}

describe('DvbMuxTreeView "On" box', () => {
  const onBox = (w: Wrapper, text: string) => rowOf(w, text).findAll('td.mux-tree__select-cell input')[1]

  it('disables a mux and a service at once', async () => {
    const w = await mountTree()
    await expand(w, 'Astra (1)')
    await expand(w, '11494H')
    await onBox(w, '11494H').trigger('click')
    await flushPromises()
    expect(JSON.parse(String(calls('idnode/save')[0].node))).toEqual({ uuid: 'm1', enabled: 0 })
    expect(rowOf(w, '11494H').classes()).toContain('mux-tree__row--off')
    await onBox(w, 'Das Erste HD').trigger('click')
    await flushPromises()
    expect(JSON.parse(String(calls('idnode/save')[1].node))).toEqual({ uuid: 's1', enabled: false })
  })

  it('enables an ignored mux by a click', async () => {
    const s = server()
    s.muxes[0].enabled = -1
    const w = await mountTree(s)
    await expand(w, 'Astra (1)')
    expect((onBox(w, '11494H').element as HTMLInputElement).indeterminate).toBe(true)
    await onBox(w, '11494H').trigger('click')
    await flushPromises()
    expect(JSON.parse(String(calls('idnode/save')[0].node))).toEqual({ uuid: 'm1', enabled: 1 })
  })

  it('takes back a click whose save fails, and says so', async () => {
    const s = server()
    const w = await mountTree(s)
    await expand(w, 'Astra (1)')
    await expand(w, '11494H')
    failSaves(s)
    const before = calls('mpegts/mux/grid').length
    await onBox(w, '11494H').trigger('click')
    await flushPromises()
    expect((onBox(w, '11494H').element as HTMLInputElement).checked).toBe(true)
    expect(rowOf(w, '11494H').classes()).not.toContain('mux-tree__row--off')
    expect(stubs.toast.error).toHaveBeenCalledWith('Failed to save: session expired')
    /* and the network as the server has it */
    expect(calls('mpegts/mux/grid').length).toBeGreaterThan(before)
    await onBox(w, 'Das Erste HD').trigger('click')
    await flushPromises()
    expect((onBox(w, 'Das Erste HD').element as HTMLInputElement).checked).toBe(true)
  })
})

describe('DvbMuxTreeView scan button', () => {
  const scanButton = (w: Wrapper, text: string) => rowOf(w, text).find('button.mux-tree__scan')

  it('queues a scan of the mux, as its editor does', async () => {
    const w = await mountTree()
    await expand(w, 'Astra (1)')
    await scanButton(w, '11494H').trigger('click')
    await flushPromises()
    expect(JSON.parse(String(calls('idnode/save')[0].node))).toEqual({ uuid: ['m1'], scan_state: 1 })
  })

  it('scans a mux in the idle scan queue', async () => {
    const s = server()
    s.muxes[0].scan_state = 2
    const w = await mountTree(s)
    await expand(w, 'Astra (1)')
    expect(scanButton(w, '11494H').attributes('disabled')).toBeUndefined()
  })

  it('is off for a disabled mux and in a disabled network, and says why', async () => {
    const s = server()
    s.muxes[0].enabled = 0
    s.networks[1].enabled = false
    const w = await mountTree(s)
    await expand(w, 'Astra (1)')
    expect(scanButton(w, '11494H').attributes('disabled')).toBeDefined()
    expect(scanButton(w, '11494H').attributes('title')).toContain('not enabled')
    await expand(w, 'IPTV Automatic Network')
    expect(scanButton(w, 'Channel One').attributes('disabled')).toBeDefined()
    expect(scanButton(w, 'Channel One').attributes('title')).toContain('network is disabled')
  })

  it('times the next reload when the last one is done', async () => {
    const s = server()
    const w = await mountTree(s)
    await expand(w, 'Astra (1)')
    vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout', 'setInterval', 'clearInterval'] })
    try {
      /* queued: the poll goes on */
      s.muxes[0].scan_state = 1
      await scanButton(w, '11494H').trigger('click')
      await flushPromises()
      const before = calls('mpegts/mux/grid').length
      const gate = holdMuxGrid(s)
      await vi.advanceTimersByTimeAsync(2000)
      expect(calls('mpegts/mux/grid')).toHaveLength(before + 1)
      /* a slow server: no more reloads meanwhile */
      await vi.advanceTimersByTimeAsync(6000)
      expect(calls('mpegts/mux/grid')).toHaveLength(before + 1)
      gate.stop()
      await flushPromises()
      expect(calls('mpegts/mux/grid')).toHaveLength(before + 1)
      await vi.advanceTimersByTimeAsync(2000)
      expect(calls('mpegts/mux/grid')).toHaveLength(before + 2)
    } finally {
      vi.useRealTimers()
    }
  })

  it('stops reloading when the scanned mux is back in the idle queue', async () => {
    const s = server()
    const w = await mountTree(s)
    await expand(w, 'Astra (1)')
    vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout', 'setInterval', 'clearInterval'] })
    try {
      s.muxes[0].scan_state = 1
      await scanButton(w, '11494H').trigger('click')
      await flushPromises()
      const before = calls('mpegts/mux/grid').length
      /* scanned: the mux rotates in the idle queue again */
      s.muxes[0].scan_state = 2
      await vi.advanceTimersByTimeAsync(2000)
      expect(calls('mpegts/mux/grid')).toHaveLength(before + 1)
      await vi.advanceTimersByTimeAsync(20000)
      expect(calls('mpegts/mux/grid')).toHaveLength(before + 1)
    } finally {
      vi.useRealTimers()
    }
  })
})

describe('DvbMuxTreeView Scan of the selection', () => {
  const select = (w: Wrapper, text: string) => rowOf(w, text).findAll('td.mux-tree__select-cell input')[0].trigger('click')

  async function mountTwoSelected(s: FakeServer) {
    const w = await mountTree(s)
    await expand(w, 'Astra (1)')
    await expand(w, 'IPTV Automatic Network')
    await select(w, '11494H')
    await select(w, 'Channel One')
    return w
  }

  it('queues all the muxes in one request', async () => {
    const w = await mountTwoSelected(server())
    await button(w, 'Scan').trigger('click')
    await flushPromises()
    const saves = calls('idnode/save')
    expect(saves).toHaveLength(1)
    expect(JSON.parse(String(saves[0].node))).toEqual({ uuid: ['m1', 'i1'], scan_state: 1 })
  })

  it('leaves out a mux that is not enabled', async () => {
    const s = server()
    s.muxes[1].enabled = -1
    const w = await mountTwoSelected(s)
    await button(w, 'Scan').trigger('click')
    await flushPromises()
    expect(JSON.parse(String(calls('idnode/save')[0].node)).uuid).toEqual(['m1'])
  })

  it('follows the scans after a save that failed', async () => {
    const s = server()
    const w = await mountTwoSelected(s)
    vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout', 'setInterval', 'clearInterval'] })
    try {
      s.gate = (ep) => (ep === 'idnode/save' ? Promise.reject(new Error('timeout')) : undefined)
      s.muxes[0].scan_state = 1
      await button(w, 'Scan').trigger('click')
      await flushPromises()
      const before = calls('mpegts/mux/grid').length
      await vi.advanceTimersByTimeAsync(2000)
      expect(calls('mpegts/mux/grid').length).toBeGreaterThan(before)
    } finally {
      vi.useRealTimers()
    }
  })
})

/* a notification, and the second the view collects them for */
async function notify(cls: string, msg: Row) {
  vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout'] })
  try {
    fireComet(cls, msg)
    await vi.advanceTimersByTimeAsync(1500)
  } finally {
    vi.useRealTimers()
  }
  await flushPromises()
}

describe('DvbMuxTreeView live updates', () => {
  it('loads the network of a changed mux again, not the others', async () => {
    const s = server()
    const w = await mountTree(s)
    await expand(w, 'Astra (1)')
    await expand(w, 'IPTV Automatic Network')
    const before = calls('mpegts/mux/grid').length
    s.muxes[0].name = '11495H'
    await notify('mpegts_mux', { change: ['m1'] })
    expect(calls('mpegts/mux/grid')).toHaveLength(before + 1)
    expect(shows(w, '11495H')).toBe(true)
  })

  it('loads the networks again when one changed', async () => {
    const s = server()
    const w = await mountTree(s)
    s.networks[0].networkname = 'Astra 19.2E'
    await notify('mpegts_network', { change: ['n1'] })
    expect(shows(w, 'Astra 19.2E')).toBe(true)
  })

  it('loads the services of a network again for a change of one of them only', async () => {
    const s = server()
    s.networks.push({ uuid: 'n3', networkname: 'ASTRA', num_mux: 1, num_svc: 1, num_chn: 0 })
    s.muxes.push({
      uuid: 'm3', name: '10744H', network: 'ASTRA', network_uuid: 'n3', tstype: 0,
      num_svc: 1, num_chn: 0, scan_state: 0, enabled: 1,
    })
    s.services.push({ uuid: 's3', svcname: 'Other', sid: 1, network: 'ASTRA', multiplex_uuid: 'm3', enabled: true })
    const w = await mountTree(s)
    await expand(w, 'Astra (1)')
    await expand(w, '11494H')
    const before = calls('mpegts/service/grid').length
    /* a change of the other network's service: not one of Astra's */
    await notify('service', { change: ['s3'] })
    expect(calls('mpegts/service/grid')).toHaveLength(before)
    await notify('service', { change: ['s1'] })
    expect(calls('mpegts/service/grid')).toHaveLength(before + 1)
  })
})

describe('DvbMuxTreeView with many muxes', () => {
  it('renders the rows in view only, a spacer for the rest', async () => {
    const s = server()
    manyMuxes(s, 1000)
    const w = await mountTree(s)
    await expand(w, 'Astra (1001)')
    const rows = w.findAll('tbody tr.mux-tree__row')
    expect(rows.length).toBeGreaterThan(20)
    expect(rows.length).toBeLessThan(150)
    expect(w.find('tbody tr.mux-tree__pad').exists()).toBe(true)
  })
})

describe('DvbMuxTreeView what the grid had', () => {
  it('opens the help page of the muxes', async () => {
    spies.helpToggle.mockClear()
    const w = await mountTree()
    await w.find('button.mux-tree__help').trigger('click')
    expect(spies.helpToggle).toHaveBeenCalledWith('class/mpegts_mux')
  })

  it('plays a mux and a service', async () => {
    spies.openPlay.mockClear()
    const w = await mountTree()
    await expand(w, 'Astra (1)')
    await expand(w, '11494H')
    const playOf = (text: string) => rowOf(w, text).find('button[aria-label="Play this stream"]')
    await playOf('11494H').trigger('click')
    expect(spies.openPlay).toHaveBeenLastCalledWith(`stream/mux/m1?title=${encodeURIComponent('11494H / Astra')}`)
    await playOf('Das Erste HD').trigger('click')
    expect(spies.openPlay).toHaveBeenLastCalledWith(`stream/service/s1?title=${encodeURIComponent('Das Erste HD')}`)
    /* a click on Play selects nothing */
    expect(w.findAll('tr.mux-tree__row--selected')).toHaveLength(0)
  })

  it('shows the frequency of a transponder with its unit', async () => {
    const w = await mountTree()
    await expand(w, 'Astra (1)')
    expect(rowOf(w, '11494H').find('td.mux-tree__name').attributes('title')).toContain('11494000 kHz')
  })
})

describe('DvbMuxTreeView keyboard', () => {
  it('is a treegrid: rows with level, open state and selection', async () => {
    const w = await mountTree()
    await expand(w, 'Astra (1)')
    expect(w.find('table').attributes('role')).toBe('treegrid')
    const net = rowOf(w, 'Astra (1)')
    expect(net.attributes('aria-level')).toBe('1')
    expect(net.attributes('aria-expanded')).toBe('true')
    const mux = rowOf(w, '11494H')
    expect(mux.attributes('aria-level')).toBe('2')
    expect(mux.attributes('aria-expanded')).toBe('false')
    expect(mux.attributes('aria-selected')).toBe('false')
    await mux.trigger('click')
    expect(rowOf(w, '11494H').attributes('aria-selected')).toBe('true')
  })

  it('works with the keys: move, open, select, edit', async () => {
    const w = await mountTree()
    const table = w.find('table')
    await table.trigger('focus')
    expect(table.attributes('aria-activedescendant')).toBe('mux-tree-row-n1')
    /* Right opens the network, Down goes to its mux */
    await table.trigger('keydown', { key: 'ArrowRight' })
    await flushPromises()
    expect(shows(w, '11494H')).toBe(true)
    await table.trigger('keydown', { key: 'ArrowDown' })
    expect(table.attributes('aria-activedescendant')).toBe('mux-tree-row-m1')
    await table.trigger('keydown', { key: ' ' })
    expect(rowOf(w, '11494H').attributes('aria-selected')).toBe('true')
    await table.trigger('keydown', { key: 'Enter' })
    expect(stubs.captured.editor.uuid).toBe('m1')
    /* Left: back to the network row */
    await table.trigger('keydown', { key: 'ArrowLeft' })
    expect(table.attributes('aria-activedescendant')).toBe('mux-tree-row-n1')
  })
})

describe('DvbMuxTreeView scan of a whole network', () => {
  const netScan = (w: Wrapper) => rowOf(w, 'Astra (').find('button.mux-tree__scan')

  it('is off for a disabled network, and says why', async () => {
    const s = server()
    s.networks[0].enabled = false
    const w = await mountTree(s)
    expect(netScan(w).attributes('disabled')).toBeDefined()
    expect(netScan(w).attributes('title')).toContain('network is disabled')
  })

  it('counts the muxes queued or scanning, not those in the idle queue, and ends with them', async () => {
    const s = server()
    s.muxes.push({
      uuid: 'm2', name: '11523H', network: 'Astra', network_uuid: 'n1', tstype: 0,
      num_svc: 0, num_chn: 0, scan_state: 0, scan_result: 1, enabled: 1,
    })
    s.networks[0].num_mux = 2
    const w = await mountTree(s)
    await expand(w, 'Astra (2)')
    vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout', 'setInterval', 'clearInterval'] })
    try {
      /* the network scan queues one, the other rotates in the idle queue */
      s.muxes[0].scan_state = 1
      s.muxes[2].scan_state = 2
      await netScan(w).trigger('click')
      await flushPromises()
      expect(calls('mpegts/network/scan')).toHaveLength(1)
      expect(rowOf(w, 'Astra (2)').text()).toContain('scanning… · 1')
      /* scanned: both in the idle queue now */
      s.muxes[0].scan_state = 2
      await vi.advanceTimersByTimeAsync(2000)
      await flushPromises()
      expect(rowOf(w, 'Astra (2)').text()).not.toContain('scanning')
      const n = calls('mpegts/mux/grid').length
      await vi.advanceTimersByTimeAsync(20000)
      expect(calls('mpegts/mux/grid')).toHaveLength(n)
    } finally {
      vi.useRealTimers()
    }
  })
})

describe('DvbMuxTreeView add an input stream', () => {
  const addButton = (w: Wrapper, text: string) => rowOf(w, text).find('button.mux-tree__addstream')

  /* open the dialog of the row and answer it (the stub: ISI 5) */
  async function addTo(w: Wrapper, text: string) {
    await addButton(w, text).trigger('click')
    await w.find('button.add-stream-stub').trigger('click')
    await flushPromises()
  }

  it('offers it for a DVB-S2 transponder, not for its input streams or an IPTV mux', async () => {
    const s = server()
    multistream(s)
    const w = await mountTree(s)
    await expand(w, 'Astra (4)')
    await expand(w, 'IPTV Automatic Network')
    expect(addButton(w, '11494H').exists()).toBe(true)
    expect(addButton(w, '12226V').exists()).toBe(true)
    expect(addButton(w, 'ISI 14').exists()).toBe(false)
    expect(addButton(w, 'Channel One').exists()).toBe(false)
  })

  it('adds the stream and shows it below its transponder', async () => {
    const s = server()
    multistream(s)
    s.addStream = [{ uuid: 't1c' }]
    const w = await mountTree(s)
    await expand(w, 'Astra (4)')
    await expand(w, '12226V')
    await addButton(w, '12226V').trigger('click')
    expect(w.find('button.add-stream-stub').text()).toBe('12226V')
    s.muxes.push({
      uuid: 't1c', name: '12226V ISI 5', network: 'Astra', network_uuid: 'n1', tstype: 7, parent_mux: 't1',
      stream_id: 5, num_svc: 0, num_chn: 0, scan_state: 1, enabled: 1,
    })
    await w.find('button.add-stream-stub').trigger('click')
    await flushPromises()
    expect(calls('mpegts/mux/add_stream')).toEqual([{ uuid: 't1', stream_id: 5 }])
    /* the transponder was closed: open again for the new stream */
    expect(shows(w, 'ISI 5')).toBe(true)
    expect(w.find('button.add-stream-stub').exists()).toBe(false)
  })

  it('asks before a mux with services becomes a transponder', async () => {
    const s = server()
    s.addStream = [{ error: 'the mux has services', services: 1 }, { uuid: 'x1' }]
    const w = await mountTree(s)
    await expand(w, 'Astra (1)')
    await addTo(w, '11494H')
    expect(calls('mpegts/mux/add_stream')).toEqual([
      { uuid: 'm1', stream_id: 5 },
      { uuid: 'm1', stream_id: 5, force: 1 },
    ])
    expect(stubs.toast.error).not.toHaveBeenCalled()
  })

  it('says what the server has against it', async () => {
    const s = server()
    s.addStream = [{ error: 'the mux is this input stream' }]
    const w = await mountTree(s)
    await expand(w, 'Astra (1)')
    await addTo(w, '11494H')
    expect(stubs.toast.error).toHaveBeenCalledWith('Failed to add the input stream: the mux is this input stream')
  })
})
