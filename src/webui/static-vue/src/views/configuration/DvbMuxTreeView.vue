<!--
  SPDX-License-Identifier: GPL-3.0-or-later
  Copyright (C) 2026 Tvheadend contributors
-->
<script setup lang="ts">
/*
 * Configuration → DVB Inputs → Muxes.
 *
 * The muxes as a tree: network → transponder → the sub-muxes carried in
 * it (the input streams of a multistream transponder).
 *
 * The networks come from `mpegts/network/grid` (all of them: the muxes
 * of an IPTV network have no sub-muxes and are a flat list), the muxes
 * of a network from `mpegts/mux/grid` when it is first expanded. The
 * tree is built in `muxTree.ts`. "Reload" loads all again.
 *
 * The flat grid of all muxes is the list view of this tab
 * (DvbMuxesView.vue, route `config-dvb-mux-list`): sortable tuning
 * columns, Hide mode, column picker, phone layout. The toolbar
 * switches to it.
 */
import {
  computed,
  onMounted,
  ref,
  shallowRef,
} from 'vue'
import { useRouter } from 'vue-router'
import Button from 'primevue/button'
import {
  ChevronDown,
  ChevronRight,
  List,
  RefreshCw,
} from 'lucide-vue-next'
import { apiCall } from '@/api/client'
import { useI18n } from '@/composables/useI18n'
import { apiErrorMessage } from '@/utils/apiErrorMessage'
import {
  buildMuxTree,
  countMuxes,
  exactMatch,
  expandableKeys,
  MUX_FIELDS,
  MUX_TYPE_TS,
  muxTypeName,
  scanQueued,
  scanResultName,
  SCAN_STATE_ACTIVE,
  SCAN_STATE_IPEND,
  subMuxScan,
  subtreeCounts,
  tuningLabel,
  type MuxRow,
  type MuxTreeNode,
} from './muxTree'

const { t } = useI18n()
const router = useRouter()

interface NetworkEntry {
  uuid: string
  name: string
  numMux: number
  numSvc: number
  numChn: number
}

interface GridResponse<T> {
  entries?: T[]
}

interface FlatRow {
  key: string
  depth: number
  network?: NetworkEntry
  node?: MuxTreeNode
  hasChildren: boolean
  open: boolean
  loading?: boolean
  /* a network or transponder row: its name uses the type column too (a
   * transponder: its tuning there) */
  wide?: boolean
}

const networks = ref<NetworkEntry[]>([])
/* What is loaded is replaced as a whole, shallow: thousands of rows are
 * not made reactive one by one */
const trees = shallowRef<Record<string, MuxTreeNode[]>>({})
const loadingNets = ref(new Set<string>())
const expanded = ref(new Set<string>())
/* the networks loaded once: an empty tree says why */
const networksLoaded = ref(false)

/* Errors: a failed load stays in a line above the tree until that load
 * succeeds (by what failed) */
const loadErrors = ref(new Map<string, string>())
const loadError = computed(() => [...new Set(loadErrors.value.values())].join(' · '))

function setLoadError(what: string, e?: unknown) {
  const next = new Map(loadErrors.value)
  if (e === undefined) {
    if (!next.delete(what)) return
  } else next.set(what, apiErrorMessage(e))
  loadErrors.value = next
}

/* Loads: one request of a kind per network at a time. A call while one
 * runs makes it run once more afterwards and returns when that is done:
 * the answers come in order (the later state wins), and a burst of calls
 * gives two requests at most. */
function serialized<T>(run: (arg: T) => Promise<void>, key: (arg: T) => string): (arg: T) => Promise<void> {
  const running = new Map<string, { again: boolean; arg: T; done: Promise<void> }>()
  return (arg: T) => {
    const k = key(arg)
    const cur = running.get(k)
    if (cur) {
      cur.again = true
      cur.arg = arg
      return cur.done
    }
    const entry = { again: true, arg, done: Promise.resolve() }
    running.set(k, entry)
    entry.done = (async () => {
      try {
        while (entry.again) {
          entry.again = false
          await run(entry.arg)
        }
      } finally {
        running.delete(k)
      }
    })()
    return entry.done
  }
}

const loadNetworks = serialized<void>(fetchNetworks, () => 'networks')

async function fetchNetworks() {
  try {
    const grid = await apiCall<GridResponse<Record<string, unknown>>>('mpegts/network/grid', {
      start: 0,
      limit: 10000,
    })
    networks.value = (grid.entries ?? [])
      .map((r) => ({
        uuid: String(r.uuid),
        name: String(r.networkname ?? r.uuid),
        numMux: Number(r.num_mux ?? 0),
        numSvc: Number(r.num_svc ?? 0),
        numChn: Number(r.num_chn ?? 0),
      }))
      .sort((a, b) => a.name.localeCompare(b.name, undefined, { numeric: true }))
    networksLoaded.value = true
    setLoadError('networks')
  } catch (e) {
    setLoadError('networks', e)
  }
}

/* A string filter of a grid is an extended regular expression on the
 * server (`api_idnode.c`), so the values are escaped and anchored. */

/* the networks whose nodes open when their muxes are loaded */
const expandOnLoad = new Set<string>()
const loadMuxesOf = serialized(fetchMuxes, (net: NetworkEntry) => net.uuid)

/* the muxes of a network (expandNew: then open all its nodes) */
function loadMuxes(net: NetworkEntry, expandNew: boolean): Promise<void> {
  if (expandNew) expandOnLoad.add(net.uuid)
  return loadMuxesOf(net)
}

/* the muxes of a network, by its uuid */
async function fetchMuxes(net: NetworkEntry) {
  loadingNets.value = withKey(loadingNets.value, net.uuid, true)
  try {
    const grid = await apiCall<GridResponse<MuxRow>>('mpegts/mux/grid', {
      start: 0,
      limit: 100000,
      hidemode: 'none',
      sort: 'name',
      dir: 'ASC',
      list: MUX_FIELDS,
      filter: [{ type: 'string', field: 'network_uuid', value: exactMatch(net.uuid) }],
    })
    const rows = (grid.entries ?? []).filter((r) => r.network_uuid === net.uuid)
    const tree = buildMuxTree(rows)
    trees.value = { ...trees.value, [net.uuid]: tree }
    if (expandOnLoad.delete(net.uuid)) {
      const next = new Set(expanded.value)
      for (const k of expandableKeys(tree)) next.add(k)
      expanded.value = next
    }
    setLoadError(`muxes:${net.uuid}`)
  } catch (e) {
    setLoadError(`muxes:${net.uuid}`, e)
  } finally {
    loadingNets.value = withKey(loadingNets.value, net.uuid, false)
  }
}

/* a copy of the set with the key in it or not (the sets are replaced, so
 * their refs trigger) */
function withKey(set: Set<string>, key: string, on: boolean): Set<string> {
  const next = new Set(set)
  if (on) next.add(key)
  else next.delete(key)
  return next
}

function setOpen(key: string, open: boolean) {
  expanded.value = withKey(expanded.value, key, open)
}

async function toggleNetwork(net: NetworkEntry, open: boolean) {
  setOpen(net.uuid, open)
  if (open && !trees.value[net.uuid]) await loadMuxes(net, true)
}

function toggle(row: FlatRow) {
  if (row.network) {
    void toggleNetwork(row.network, !row.open)
    return
  }
  setOpen(row.key, !row.open)
}

async function expandAll() {
  const next = new Set(expanded.value)
  for (const n of networks.value) if (n.numMux > 0) next.add(n.uuid)
  expanded.value = next
  await Promise.all(
    networks.value.filter((n) => n.numMux > 0 && !trees.value[n.uuid]).map((n) => loadMuxes(n, false)),
  )
  const all = new Set(expanded.value)
  for (const tree of Object.values(trees.value)) for (const k of expandableKeys(tree)) all.add(k)
  expanded.value = all
}

function collapseAll() {
  expanded.value = new Set()
}

/* the flat grid of all muxes: the list view of this tab */
function showList() {
  router.push({ name: 'config-dvb-mux-list' }).catch(() => {})
}

/* no muxes at all (no networks, or none with a mux) */
const noMuxes = computed(() => networksLoaded.value && networks.value.every((n) => n.numMux === 0))

/* all again: what is loaded is replaced in place (the open rows stay and
 * do not fall back to "Loading…"), what belonged to a network that is
 * gone is dropped, what is open but failed to load is tried again */
async function reload() {
  await loadNetworks()
  const known = new Set(networks.value.map((n) => n.uuid))
  const keep = <V>(m: Record<string, V>) => Object.fromEntries(Object.entries(m).filter(([u]) => known.has(u)))
  trees.value = keep(trees.value)
  const wanted = (n: NetworkEntry) =>
    !!trees.value[n.uuid] || (n.numMux > 0 && expanded.value.has(n.uuid))
  await Promise.all(networks.value.filter(wanted).map((n) => loadMuxes(n, false)))
}

const visible = computed<FlatRow[]>(() => {
  const out: FlatRow[] = []
  const walk = (nodes: MuxTreeNode[], depth: number) => {
    for (const node of nodes) {
      const open = expanded.value.has(node.key)
      const wide = depth === 1 && !node.orphan && (node.row.tstype ?? MUX_TYPE_TS) === MUX_TYPE_TS
      out.push({ key: node.key, depth, node, hasChildren: node.children.length > 0, open, wide })
      if (!open) continue
      walk(node.children, depth + 1)
    }
  }
  for (const net of networks.value) {
    const open = expanded.value.has(net.uuid)
    out.push({ key: net.uuid, depth: 0, network: net, hasChildren: net.numMux > 0, open, wide: true })
    if (!open) continue
    if (loadingNets.value.has(net.uuid) && !trees.value[net.uuid])
      out.push({ key: `${net.uuid}:loading`, depth: 1, hasChildren: false, open: false, loading: true })
    walk(trees.value[net.uuid] ?? [], 1)
  }
  return out
})

const muxTotal = computed(() =>
  Object.values(trees.value).reduce((n, tree) => n + countMuxes(tree), 0),
)

/* queued or scanning: in place of the result (the idle queue: next to it) */
function scanStateLabel(row: MuxRow): string {
  if (!scanQueued(row)) return ''
  return row.scan_state === SCAN_STATE_ACTIVE ? t('scanning…') : t('queued')
}

function enabledLabel(row: MuxRow): string {
  if (row.enabled === 0) return t('disabled')
  if (row.enabled === -1) return t('ignored')
  return ''
}

/* the full name, the provider network and the tuning */
function nameTitle(r: FlatRow): string | undefined {
  if (r.network) return r.network.name
  const row = r.node?.row
  if (!row) return undefined
  return [row.name, r.wide ? tuningLabel(row) : '', row.pnetwork_name].filter(Boolean).join(' — ')
}

/* Services and Channels: a mux with sub-muxes counts all levels below */
function countLabel(node: MuxTreeNode, what: 'svc' | 'chn'): string {
  if (!node.children.length) return String((what === 'svc' ? node.row.num_svc : node.row.num_chn) ?? '')
  return String(subtreeCounts(node)[what])
}

function countTitle(node: MuxTreeNode, what: 'svc' | 'chn'): string | undefined {
  if (!node.children.length) return undefined
  const own = (what === 'svc' ? node.row.num_svc : node.row.num_chn) ?? 0
  const all = subtreeCounts(node)[what]
  return t('{0} own + {1} in sub-muxes', own, all - own)
}

function scanSummary(node: MuxTreeNode): string {
  const s = subMuxScan(node)
  return `${s.ok}/${s.total}`
}

function indent(depth: number): Record<string, string> {
  return { paddingLeft: `calc(${depth} * 1.25rem + var(--tvh-space-2))` }
}

onMounted(loadNetworks)
</script>

<template>
  <section class="mux-tree">
    <div class="mux-tree__toolbar">
      <Button severity="secondary" outlined size="small" @click="expandAll">
        {{ t('Expand all') }}
      </Button>
      <Button severity="secondary" outlined size="small" @click="collapseAll">
        {{ t('Collapse all') }}
      </Button>
      <Button severity="secondary" outlined size="small" @click="reload">
        <RefreshCw :size="14" :stroke-width="2" />
        {{ t('Reload') }}
      </Button>
      <span class="mux-tree__sep" />
      <Button
        severity="secondary"
        outlined
        size="small"
        :title="t('Show all muxes in one flat grid: sortable tuning columns, column picker')"
        @click="showList"
      >
        <List :size="14" :stroke-width="2" />
        {{ t('List view') }}
      </Button>
      <span class="mux-tree__count">
        {{ t('{0} networks, {1} muxes loaded', networks.length, muxTotal) }}
      </span>
    </div>
    <p v-if="loadError" class="mux-tree__error" role="alert">{{ t('Failed to load:') }} {{ loadError }}</p>
    <div class="mux-tree__scroll">
      <table class="mux-tree__table" :aria-label="t('Muxes')">
        <colgroup>
          <col />
          <col class="mux-tree__col-type" />
          <col class="mux-tree__col-scan" />
          <col class="mux-tree__col-count" />
          <col class="mux-tree__col-count" />
        </colgroup>
        <thead>
          <tr>
            <th class="mux-tree__name">{{ t('Name') }}</th>
            <th>{{ t('Type') }}</th>
            <th>{{ t('Scan result') }}</th>
            <th class="mux-tree__num">{{ t('Services') }}</th>
            <th class="mux-tree__num">{{ t('Channels') }}</th>
          </tr>
        </thead>
        <tbody>
          <tr
            v-for="r in visible"
            :key="r.key"
            class="mux-tree__row"
            :class="{
              'mux-tree__row--network': !!r.network,
              'mux-tree__row--off': !!r.node && r.node.row.enabled !== 1,
            }"
          >
            <!-- network and transponder rows: the name uses the columns before the scan result -->
            <td
              class="mux-tree__name"
              :colspan="r.wide ? 2 : undefined"
              :style="indent(r.depth)"
              :title="nameTitle(r)"
            >
              <button
                v-if="r.hasChildren"
                type="button"
                class="mux-tree__toggle"
                :aria-expanded="r.open"
                :aria-label="r.open ? t('Collapse') : t('Expand')"
                @click.stop="toggle(r)"
                @dblclick.stop
              >
                <ChevronDown v-if="r.open" :size="14" :stroke-width="2" />
                <ChevronRight v-else :size="14" :stroke-width="2" />
              </button>
              <span v-else class="mux-tree__toggle-space" />
              <template v-if="r.network">
                {{ r.network.name }}
                <span class="mux-tree__muted">({{ r.network.numMux }})</span>
              </template>
              <template v-else-if="r.loading">
                <span class="mux-tree__muted">{{ t('Loading…') }}</span>
              </template>
              <template v-else-if="r.node">
                <template v-if="r.node.label">{{ r.node.label }}</template>
                <span v-else class="mux-tree__muted">–</span>
                <span v-if="r.node.row.multistream" class="mux-tree__badge">{{ t('multistream') }}</span>
                <span v-if="r.node.orphan" class="mux-tree__badge mux-tree__badge--warn">{{ t('transponder missing') }}</span>
                <span v-if="enabledLabel(r.node.row)" class="mux-tree__badge">{{ enabledLabel(r.node.row) }}</span>
                <span v-if="r.wide" class="mux-tree__tuning">{{ tuningLabel(r.node.row) }}</span>
                <!-- after the tuning of a transponder, after the name of a sub-mux -->
                <span v-if="r.node.row.pnetwork_name" class="mux-tree__provider">
                  {{ r.node.row.pnetwork_name }}
                </span>
              </template>
            </td>
            <template v-if="r.node">
              <td v-if="!r.wide">{{ muxTypeName(r.node.row.tstype) }}</td>
              <td>
                <span v-if="scanStateLabel(r.node.row)" class="mux-tree__busy">
                  {{ scanStateLabel(r.node.row) }}
                </span>
                <template v-else>{{ scanResultName(r.node.row.scan_result) }}</template>
                <span
                  v-if="r.node.row.scan_state === SCAN_STATE_IPEND"
                  class="mux-tree__muted"
                  :title="t('In the idle scan queue of the network; a scan asked for here goes first')"
                >
                  · {{ t('idle queue') }}
                </span>
                <span v-if="r.node.children.length" class="mux-tree__muted" :title="t('Sub-muxes scanned with success')">
                  · {{ scanSummary(r.node) }}
                </span>
              </td>
              <td class="mux-tree__num" :title="countTitle(r.node, 'svc')">{{ countLabel(r.node, 'svc') }}</td>
              <td class="mux-tree__num" :title="countTitle(r.node, 'chn')">{{ countLabel(r.node, 'chn') }}</td>
            </template>
            <template v-else-if="r.network">
              <td>
              </td>
              <td class="mux-tree__num">{{ r.network.numSvc }}</td>
              <td class="mux-tree__num">{{ r.network.numChn }}</td>
            </template>
            <td v-else colspan="4" />
          </tr>
        </tbody>
      </table>
      <p v-if="noMuxes" class="mux-tree__empty">
        {{
          t(
            'No muxes defined. Add one against an existing network — or trigger a network scan from the Networks page to populate them automatically.',
          )
        }}
      </p>
    </div>
  </section>
</template>

<style scoped>
.mux-tree {
  display: flex;
  flex-direction: column;
  flex: 1 1 auto;
  min-height: 0;
  gap: var(--tvh-space-2);
}

.mux-tree__toolbar {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: var(--tvh-space-2);
  padding: var(--tvh-space-2) 0;
}

.mux-tree__count {
  margin-left: auto;
  color: var(--tvh-text-muted);
  font-size: var(--tvh-text-sm);
}

.mux-tree__empty {
  color: var(--tvh-text-muted);
  text-align: center;
  padding: var(--tvh-space-6);
}

.mux-tree__error {
  color: var(--tvh-error);
  font-size: var(--tvh-text-sm);
}

.mux-tree__scroll {
  flex: 1 1 auto;
  min-height: 0;
  overflow: auto;
  border: 1px solid var(--tvh-border);
  border-radius: var(--tvh-radius-sm);
  background: var(--tvh-bg-surface);
}

/* fixed column widths: expanding a node does not move the columns */
.mux-tree__table {
  width: 100%;
  min-width: 900px;
  table-layout: fixed;
  border-collapse: collapse;
  font-size: var(--tvh-text-sm);
}

.mux-tree__col-type {
  width: 88px;
}

.mux-tree__col-scan {
  width: 128px;
}

.mux-tree__col-count {
  width: 78px;
}

.mux-tree__sep {
  width: 1px;
  height: 1.4rem;
  background: var(--tvh-border);
  margin: 0 var(--tvh-space-1);
}

.mux-tree__busy {
  color: var(--tvh-primary);
}

.mux-tree__table th {
  position: sticky;
  top: 0;
  z-index: 1;
  background: var(--tvh-bg-surface);
  border-bottom: 1px solid var(--tvh-border);
  text-align: left;
  font-weight: 600;
  padding: 0 var(--tvh-space-2);
  height: var(--tvh-row-height);
  white-space: nowrap;
}

.mux-tree__table td {
  padding: 0 var(--tvh-space-2);
  height: var(--tvh-row-height);
  border-bottom: 1px solid color-mix(in srgb, var(--tvh-border) 50%, transparent);
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
}

/* after the general th rule: numeric headers align with their values */
.mux-tree__table th.mux-tree__num {
  text-align: right;
}

.mux-tree__provider {
  margin-left: var(--tvh-space-2);
  color: var(--tvh-text-muted);
  font-weight: normal;
}

.mux-tree__tuning {
  margin-left: var(--tvh-space-3);
  color: var(--tvh-text-muted);
  font-size: var(--tvh-text-xs);
  font-weight: normal;
  font-variant-numeric: tabular-nums;
}

.mux-tree__table tbody tr {
  cursor: default;
  user-select: none;
}

.mux-tree__table tbody tr.mux-tree__row:hover {
  background: color-mix(in srgb, var(--tvh-primary) var(--tvh-hover-strength), transparent);
}

.mux-tree__num {
  text-align: right;
  font-variant-numeric: tabular-nums;
}

.mux-tree__row--network td {
  font-weight: 600;
}

.mux-tree__row--off td {
  color: var(--tvh-text-muted);
}

.mux-tree__toggle,
.mux-tree__toggle-space {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  width: 1.25rem;
  height: 1.25rem;
  margin-right: var(--tvh-space-1);
  vertical-align: middle;
}

.mux-tree__toggle {
  padding: 0;
  border: none;
  background: none;
  color: var(--tvh-text);
  cursor: pointer;
  border-radius: var(--tvh-radius-sm);
}

.mux-tree__toggle:focus-visible {
  outline: 2px solid var(--tvh-focus);
}

.mux-tree__muted {
  color: var(--tvh-text-muted);
  font-weight: normal;
}

.mux-tree__badge {
  margin-left: var(--tvh-space-2);
  padding: 0 var(--tvh-space-1);
  border: 1px solid var(--tvh-border);
  border-radius: var(--tvh-radius-sm);
  font-size: var(--tvh-text-xs);
  font-style: normal;
  font-weight: normal;
  color: var(--tvh-text-muted);
}

.mux-tree__badge--warn {
  border-color: var(--tvh-warning);
  color: var(--tvh-warning);
}
</style>
