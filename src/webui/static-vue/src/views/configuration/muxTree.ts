// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Tvheadend contributors

/*
 * The muxes of a network as a tree: transponders and the sub-muxes
 * carried in them (the input streams of a multistream transponder).
 *
 * A sub-mux names its transponder in `parent_mux` (uuid, DVB muxes
 * only); the depth is not limited here.
 *
 * Pure functions, no Vue: the view (DvbMuxTreeView.vue) renders the
 * nodes and keeps the expanded state. The labels go through the static
 * `t()`.
 */
import { t } from '@/composables/useI18n'

/* the fields of an `api/mpegts/mux/grid` row the tree uses */
export interface MuxRow {
  uuid: string
  name?: string
  network_uuid?: string
  parent_mux?: string
  multistream?: boolean
  tstype?: number
  stream_id?: number
  [key: string]: unknown
}

/* the fields of the mux grid the tree uses: the `list` of its requests,
 * the server sends only these (and the uuid) */
export const MUX_FIELDS = [
  'name', 'network_uuid', 'parent_mux', 'multistream', 'tstype', 'stream_id',
].join(',')

export interface MuxTreeNode {
  key: string
  row: MuxRow
  label: string
  /* has a parent_mux that is not among the rows */
  orphan: boolean
  isi: number | null
  children: MuxTreeNode[]
}

/* mm_type, MM_TYPE_* in src/input/mpegts.h */
export const MUX_TYPE_TS = 0
export const MUX_TYPE_ISI = 7

/* the type as the tree shows it */
const TYPE_LABELS: Record<number, () => string> = {
  [MUX_TYPE_TS]: () => t('TS'),
  [MUX_TYPE_ISI]: () => t('TS (ISI)'),
}

export function muxTypeName(type: number | undefined): string {
  return TYPE_LABELS[type ?? MUX_TYPE_TS]?.() ?? t('type {0}', type)
}

/* a string filter of the grids is a case-insensitive extended regular
 * expression on the server (`idnode_filter_add_str`): the one that finds
 * exactly this text */
export function exactMatch(text: string): string {
  return `^${containsMatch(text)}$`
}

/* ... and the one that finds the text anywhere */
export function containsMatch(text: string): string {
  return text.replace(/[.[\]()*+?{}|^$\\]/g, '\\$&')
}

/* an input stream of its own */
function isInputStream(row: MuxRow): boolean {
  return row.tstype === MUX_TYPE_ISI
}

function hasIsi(row: MuxRow): boolean {
  return typeof row.stream_id === 'number' && row.stream_id >= 0
}

const collator = new Intl.Collator(undefined, { numeric: true, sensitivity: 'base' })

/* a sub-mux is named by what carries it, then its own part: the label is
 * that part, "12226V ISI 14" below "12226V" → "ISI 14" */
export function shortName(name: string, parentName: string | undefined): string {
  if (parentName && name.startsWith(parentName + ' ') && name.length > parentName.length + 1)
    return name.slice(parentName.length + 1)
  return name
}

/* input streams by ISI, then the others by type and name */
function compareNodes(a: MuxTreeNode, b: MuxTreeNode): number {
  const ai = a.isi !== null && isInputStream(a.row)
  const bi = b.isi !== null && isInputStream(b.row)
  if (ai !== bi) return ai ? -1 : 1
  if (ai && bi && a.isi !== b.isi) return (a.isi ?? 0) - (b.isi ?? 0)
  const at = a.row.tstype ?? MUX_TYPE_TS
  const bt = b.row.tstype ?? MUX_TYPE_TS
  if (!ai && at !== bt) return at - bt
  return collator.compare(a.label, b.label)
}

function sortTree(nodes: MuxTreeNode[]): void {
  nodes.sort(compareNodes)
  for (const n of nodes) sortTree(n.children)
}

export function buildMuxTree(rows: MuxRow[]): MuxTreeNode[] {
  const byUuid = new Map(rows.map((r) => [r.uuid, r]))

  /* the parent a row hangs below, null at the top (no parent, the
   * parent is not among the rows, or the chain loops) */
  function parentOf(row: MuxRow): MuxRow | null {
    const p = row.parent_mux ? byUuid.get(row.parent_mux) : undefined
    if (!p) return null
    const seen = new Set([row.uuid])
    for (let q: MuxRow | undefined = p; q; q = q.parent_mux ? byUuid.get(q.parent_mux) : undefined) {
      if (seen.has(q.uuid)) return null
      seen.add(q.uuid)
    }
    return p
  }

  const children = new Map<string, MuxRow[]>()
  const top: MuxRow[] = []
  for (const r of rows) {
    const p = parentOf(r)
    if (!p) {
      top.push(r)
      continue
    }
    const l = children.get(p.uuid)
    if (l) l.push(r)
    else children.set(p.uuid, [r])
  }

  function build(row: MuxRow, parentName: string | undefined): MuxTreeNode {
    const name = row.name ?? row.uuid
    return {
      key: row.uuid,
      row,
      label: shortName(name, parentName),
      orphan: !!row.parent_mux && !byUuid.has(row.parent_mux),
      isi: hasIsi(row) ? (row.stream_id as number) : null,
      children: (children.get(row.uuid) ?? []).map((c) => build(c, name)),
    }
  }

  const tree = top.map((r) => build(r, undefined))
  sortTree(tree)
  return tree
}

/* every node key with children: for "expand all" */
export function expandableKeys(nodes: MuxTreeNode[], out: string[] = []): string[] {
  for (const n of nodes) {
    if (n.children.length) {
      out.push(n.key)
      expandableKeys(n.children, out)
    }
  }
  return out
}

/* the number of muxes in the tree */
export function countMuxes(nodes: MuxTreeNode[]): number {
  let n = 0
  for (const x of nodes) n += 1 + countMuxes(x.children)
  return n
}
