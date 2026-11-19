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

const stubs = await vi.hoisted(() => import('@/test/__helpers__/gridViewTestUtils'))
vi.mock('@/api/client', () => stubs.apiClient)
vi.mock('vue-router', () => stubs.vueRouter)

import DvbMuxTreeView from '../DvbMuxTreeView.vue'

stubs.resetEachTest()

type Row = Record<string, unknown>

interface FakeServer {
  networks: Row[]
  muxes: Row[]
  services: Row[]
  /* when set: an answer waits for the promise it returns */
  gate?: (ep: string) => Promise<void> | undefined
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
    s.gate = undefined
    await button(w, 'Reload').trigger('click')
    await flushPromises()
    expect(w.find('.mux-tree__error').exists()).toBe(false)
    expect(shows(w, '11494H')).toBe(true)
  })
})
