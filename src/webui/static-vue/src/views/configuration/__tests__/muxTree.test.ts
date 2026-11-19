// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Tvheadend contributors

import { afterEach, describe, expect, it } from 'vitest'
import { clearLocale, switchLocale } from '@/test/__helpers__/switchLocale'
import {
  buildMuxTree,
  containsMatch,
  countMuxes,
  exactMatch,
  expandableKeys,
  muxTypeName,
  MUX_TYPE_ISI,
  shortName,
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

  it('translates the mux type', async () => {
    await switchLocale({ 'TS (ISI)': 'TS (Eingangsstrom)' })
    expect(muxTypeName(MUX_TYPE_ISI)).toBe('TS (Eingangsstrom)')
    expect(muxTypeName(0)).toBe('TS')
    expect(muxTypeName(undefined)).toBe('TS')
  })
})
