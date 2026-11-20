// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Tvheadend contributors

import { afterEach, describe, expect, it } from 'vitest'
import { clearLocale, switchLocale } from '@/test/__helpers__/switchLocale'
import {
  buildMuxTree,
  caidList,
  servicesByMux,
  serviceTypeName,
  containsMatch,
  countMuxes,
  exactMatch,
  expandableKeys,
  filterTree,
  muxSearchText,
  muxTypeName,
  scanEnabled,
  scanPending,
  scanQueued,
  scanResultName,
  MUX_TYPE_ISI,
  MUX_TYPE_T2MI,
  shortName,
  subMuxScan,
  subtreeCounts,
  tuningLabel,
  type MuxRow,
  type MuxTreeNode,
} from '../muxTree'

function labels(nodes: MuxTreeNode[]): string[] {
  return nodes.map((n) => n.label)
}

describe('buildMuxTree', () => {
  it('puts the sub-muxes below their transponder', () => {
    const rows: MuxRow[] = [
      { uuid: 'b', name: '12092H', multistream: true },
      { uuid: 'a', name: '11727V' },
      { uuid: 's2', name: '12092H ISI 2', parent_mux: 'b', tstype: MUX_TYPE_ISI, stream_id: 2 },
      { uuid: 's1', name: '12092H ISI 1', parent_mux: 'b', tstype: MUX_TYPE_ISI, stream_id: 1 },
    ]
    const tree = buildMuxTree(rows)
    expect(labels(tree)).toEqual(['11727V', '12092H'])
    expect(labels(tree[1].children)).toEqual(['ISI 1', 'ISI 2'])
    expect(countMuxes(tree)).toBe(4)
    expect(expandableKeys(tree)).toEqual(['b'])
  })

  it('puts a T2MI mux below what carries it: a transponder, or one of its input streams', () => {
    const tree = buildMuxTree([
      { uuid: 'a', name: '12285V' },
      { uuid: 'x', name: '12285V T2MI 4097', parent_mux: 'a', tstype: MUX_TYPE_T2MI, pid: 4097 },
      { uuid: 'w', name: '12285V T2MI 4096', parent_mux: 'a', tstype: MUX_TYPE_T2MI, pid: 4096 },
      { uuid: 't', name: '12606V', multistream: true },
      { uuid: 'i', name: '12606V ISI 5', parent_mux: 't', tstype: MUX_TYPE_ISI, stream_id: 5 },
      /* it has the ISI of its input stream in its tuning */
      { uuid: 'y', name: '12606V ISI 5 T2MI 4096 PLP 1', parent_mux: 'i', tstype: MUX_TYPE_T2MI, stream_id: 5, pid: 4096 },
    ])
    expect(labels(tree[0].children)).toEqual(['T2MI 4096', 'T2MI 4097'])
    expect(labels(tree[1].children)).toEqual(['ISI 5'])
    expect(labels(tree[1].children[0].children)).toEqual(['T2MI 4096 PLP 1'])
    expect(countMuxes(tree)).toBe(6)
  })

  it('sorts frequencies numerically', () => {
    const tree = buildMuxTree([
      { uuid: '1', name: '9750H' },
      { uuid: '2', name: '12092H' },
      { uuid: '3', name: '10744V' },
    ])
    expect(labels(tree)).toEqual(['9750H', '10744V', '12092H'])
  })

  it('shows the input streams of a multistream transponder by ISI', () => {
    const tree = buildMuxTree([
      { uuid: 't', name: '12226V', multistream: true },
      { uuid: 'c', name: '12226V ISI 20', parent_mux: 't', tstype: MUX_TYPE_ISI, stream_id: 20 },
      { uuid: 'a', name: '12226V ISI 3', parent_mux: 't', tstype: MUX_TYPE_ISI, stream_id: 3 },
      { uuid: 'b', name: '12226V ISI 14', parent_mux: 't', tstype: MUX_TYPE_ISI, stream_id: 14 },
    ])
    expect(labels(tree[0].children)).toEqual(['ISI 3', 'ISI 14', 'ISI 20'])
    expect(tree[0].children.map((n) => n.isi)).toEqual([3, 14, 20])
  })

  it('keeps a sub-mux whose transponder is missing at the top, marked', () => {
    const tree = buildMuxTree([{ uuid: 's', name: '12226V ISI 14', parent_mux: 'gone', tstype: MUX_TYPE_ISI, stream_id: 14 }])
    expect(labels(tree)).toEqual(['12226V ISI 14'])
    expect(tree[0].orphan).toBe(true)
  })

  it('does not loop on a parent chain that loops', () => {
    const tree = buildMuxTree([
      { uuid: 'a', name: 'A', parent_mux: 'b' },
      { uuid: 'b', name: 'B', parent_mux: 'a' },
    ])
    expect(labels(tree).sort()).toEqual(['A', 'B'])
    expect(countMuxes(tree)).toBe(2)
  })
})

describe('shortName', () => {
  it('drops the name of the transponder', () => {
    expect(shortName('12226V ISI 14', '12226V')).toBe('ISI 14')
    expect(shortName('12226V', '12226V')).toBe('12226V')
    expect(shortName('12226V ISI 14', undefined)).toBe('12226V ISI 14')
    expect(shortName('11727V ISI 1', '12226V')).toBe('11727V ISI 1')
  })
})

describe('subMuxScan', () => {
  it('counts the sub-muxes of all levels scanned with success', () => {
    const tree = buildMuxTree([
      { uuid: 't', name: '10717V', multistream: true, scan_result: 1 },
      { uuid: 'a', name: '10717V ISI 1', parent_mux: 't', tstype: MUX_TYPE_ISI, stream_id: 1, scan_result: 1 },
      { uuid: 'b', name: '10717V ISI 2', parent_mux: 't', tstype: MUX_TYPE_ISI, stream_id: 2, scan_result: 2 },
      { uuid: 'c', name: '10717V ISI 3', parent_mux: 't', tstype: MUX_TYPE_ISI, stream_id: 3, scan_result: 3 },
    ])
    expect(subMuxScan(tree[0])).toEqual({ ok: 2, total: 3 })
    expect(subMuxScan(tree[0].children[0])).toEqual({ ok: 0, total: 0 })
  })
})

describe('subtreeCounts', () => {
  it('adds the services and channels of all levels below a mux', () => {
    const tree = buildMuxTree([
      { uuid: 't', name: '12226V', multistream: true, num_svc: 0, num_chn: 0 },
      { uuid: 'i', name: '12226V ISI 14', parent_mux: 't', tstype: MUX_TYPE_ISI, stream_id: 14, num_svc: 19, num_chn: 2 },
      { uuid: 'j', name: '12226V ISI 15', parent_mux: 't', tstype: MUX_TYPE_ISI, stream_id: 15, num_svc: 12, num_chn: 1 },
    ])
    expect(subtreeCounts(tree[0])).toEqual({ svc: 31, chn: 3 })
    expect(subtreeCounts(tree[0].children[0])).toEqual({ svc: 19, chn: 2 })
  })
})

describe('tuningLabel', () => {
  it('shows the DVB-S2 tuning, PLS only when scrambled', () => {
    expect(
      tuningLabel({
        uuid: 'a', delsys: 'DVB-S2', modulation: 'PSK/8', fec: '2/3', symbolrate: 31400000,
        rolloff: '20', pilot: 'ON', pls_mode: 'GOLD', pls_code: 262140,
      }),
    ).toBe('DVB-S2 · 8PSK 2/3 · SR 31400 · PLS Gold 262140 · RO 0.20 · Pilot on')
    expect(
      tuningLabel({
        uuid: 'b', delsys: 'DVB-S2', modulation: 'PSK/8', fec: '3/4', symbolrate: 4500000,
        rolloff: 'NONE', pilot: 'AUTO', pls_mode: 'ROOT', pls_code: 1,
      }),
    ).toBe('DVB-S2 · 8PSK 3/4 · SR 4500 · Pilot auto')
  })

  it('leaves out what DVB-S and DVB-C do not have', () => {
    expect(
      tuningLabel({ uuid: 'c', delsys: 'DVB-S', modulation: 'QPSK', fec: '3/4', symbolrate: 27500000, rolloff: '35', pilot: 'AUTO' }),
    ).toBe('DVB-S · QPSK 3/4 · SR 27500')
    expect(tuningLabel({ uuid: 'd', delsys: 'DVB-C', constellation: 'QAM/256', fec: 'AUTO', symbolrate: 6900000 })).toBe(
      'DVB-C · 256QAM · SR 6900',
    )
  })
})

describe('scan state', () => {
  const row = (scan_state: number, enabled = 1) => ({ uuid: 'm', scan_state, enabled })

  it('counts a queued or running scan, not the idle queue', () => {
    expect(scanQueued(row(0))).toBe(false)
    expect(scanQueued(row(1))).toBe(true)
    expect(scanQueued(row(2))).toBe(false)
    expect(scanQueued(row(3))).toBe(true)
  })

  it('scans the enabled muxes only', () => {
    expect(scanEnabled(row(0, 1))).toBe(true)
    expect(scanEnabled(row(0, 0))).toBe(false)
    expect(scanEnabled(row(0, -1))).toBe(false)
    expect(scanEnabled({ uuid: 'm' })).toBe(true)
  })
})

describe('scanPending', () => {
  it('counts the muxes of all levels queued or being scanned, not those in the idle queue', () => {
    const tree = buildMuxTree([
      { uuid: 't', name: '12111V', multistream: true, scan_state: 0 },
      { uuid: 'i1', name: '12111V ISI 1', parent_mux: 't', tstype: MUX_TYPE_ISI, stream_id: 1, scan_state: 1 },
      { uuid: 'i3', name: '12111V ISI 3', parent_mux: 't', tstype: MUX_TYPE_ISI, stream_id: 3, scan_state: 2 },
      { uuid: 'i4', name: '12111V ISI 4', parent_mux: 't', tstype: MUX_TYPE_ISI, stream_id: 4, scan_state: 3 },
      { uuid: 'u', name: '11727V', scan_state: 1 },
    ])
    expect(scanPending(tree)).toBe(3)
    expect(scanPending([])).toBe(0)
  })
})

describe('services', () => {
  it('names the service types and the CA systems', () => {
    expect(serviceTypeName(0x19)).toBe('HD TV')
    expect(serviceTypeName(0x02)).toBe('Radio')
    expect(serviceTypeName(0x86)).toBe('0x86')
    expect(serviceTypeName(0)).toBe('')
    expect(caidList('1883:003311,1863:003343,1883:000000')).toBe('1883, 1863')
    expect(caidList('')).toBe('')
  })

  it('groups the services by mux, sorted by name', () => {
    const m = servicesByMux([
      { uuid: '1', svcname: 'ORF2', sid: 2, multiplex_uuid: 'a' },
      { uuid: '2', svcname: 'ORF1', sid: 1, multiplex_uuid: 'a' },
      { uuid: '3', svcname: 'X', sid: 9, multiplex_uuid: 'b' },
    ])
    expect(m.get('a')?.map((s) => s.svcname)).toEqual(['ORF1', 'ORF2'])
    expect(m.get('b')?.length).toBe(1)
  })
})

describe('filterTree', () => {
  const tree = buildMuxTree([
    { uuid: 't', name: '10717V', multistream: true, pnetwork_name: 'Thor', delsys: 'DVB-S2' },
    { uuid: 'r', name: '10717V ISI 171', parent_mux: 't', tstype: MUX_TYPE_ISI, stream_id: 171, pnetwork_name: 'Riks' },
    { uuid: 'n', name: '10717V ISI 172', parent_mux: 't', tstype: MUX_TYPE_ISI, stream_id: 172 },
    { uuid: 'o', name: '11034V' },
  ])
  const text = (q: string) => (n: MuxTreeNode) => muxSearchText(n).includes(q)

  it('shows a match with the nodes above it opened', () => {
    const f = filterTree(tree, text('riks'))
    expect([...f.show].sort()).toEqual(['r', 't'])
    expect(f.open.has('t')).toBe(true)
    expect(f.self.has('r')).toBe(true)
    expect(f.show.has('o')).toBe(false)
  })

  it('finds a mux by its type and tuning', () => {
    expect(filterTree(tree, text('ts (isi)')).self.size).toBe(2)
    expect([...filterTree(tree, text('dvb-s2')).self]).toEqual(['t'])
  })

  it('opens a mux for a match among its services', () => {
    const f = filterTree(tree, () => false, (n) => n.key === 'o')
    expect(f.show.has('o')).toBe(true)
    expect(f.open.has('o')).toBe(true)
  })
})

describe('exactMatch', () => {
  /* the server compiles it as an extended regular expression and
   * ignores case: JavaScript reads these the same way */
  const names = ['Astra 1 (19.2E)', 'HD+ (19.2E)', 'Hotbird [13E]', 'DVB-S [Dish 1', 'Kabel\\Wien', 'A|B', 'x{1,2}^$*?']

  it('finds the text itself, all of it', () => {
    expect(exactMatch('Astra 1 (19.2E)')).toBe('^Astra 1 \\(19\\.2E\\)$')
    for (const n of names) {
      const re = new RegExp(exactMatch(n), 'i')
      expect(re.test(n), n).toBe(true)
      expect(re.test(`${n} 2`), n).toBe(false)
      expect(re.test(`x${n}`), n).toBe(false)
    }
  })

  it('leaves a uuid as it is, between the anchors', () => {
    expect(exactMatch('0123456789abcdef0123456789abcdef')).toBe('^0123456789abcdef0123456789abcdef$')
  })
})

describe('containsMatch', () => {
  it('finds the text anywhere', () => {
    const re = new RegExp(containsMatch('2 (w'), 'i')
    expect(re.test('ORF 2 (W)')).toBe(true)
    expect(re.test('ORF 2 W')).toBe(false)
  })
})

describe('labels in the language of the UI', () => {
  afterEach(clearLocale)

  it('translates the mux type, the service type and the scan result', async () => {
    await switchLocale({ 'TS (ISI)': 'TS (Eingangsstrom)', Radio: 'Hörfunk', 'OK (partial)': 'OK (teilweise)' })
    expect(muxTypeName(MUX_TYPE_ISI)).toBe('TS (Eingangsstrom)')
    expect(muxTypeName(MUX_TYPE_T2MI)).toBe('T2MI')
    expect(muxTypeName(0)).toBe('TS')
    expect(muxTypeName(undefined)).toBe('TS')
    expect(serviceTypeName(0x02)).toBe('Hörfunk')
    expect(serviceTypeName(0x0a)).toBe('Hörfunk')
    expect(scanResultName(3)).toBe('OK (teilweise)')
    expect(scanResultName(0)).toBe('')
  })
})
