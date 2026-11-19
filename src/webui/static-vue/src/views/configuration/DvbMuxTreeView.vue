<!--
  SPDX-License-Identifier: GPL-3.0-or-later
  Copyright (C) 2026 Tvheadend contributors
-->
<script setup lang="ts">
/*
 * Configuration → DVB Inputs → Muxes.
 *
 * The muxes as a tree: network → transponder → the sub-muxes carried in
 * it (the input streams of a multistream transponder), and the services
 * of each mux.
 * A double click opens the editor of the mux, service or network.
 *
 * Rows are selected by a click (Ctrl/Cmd adds one, Shift a range) or
 * their check box; the toolbar adds a mux (to the network of the
 * selection, else one picked), edits the selection (several of a kind at
 * once) and deletes it.
 *
 * The filter finds muxes by name, type, provider network and tuning (all
 * networks are loaded for it) and by state (scan failed, disabled); the
 * rows above a match open.
 * It optionally looks in the names of the services too.
 *
 * Live: Comet notifications of muxes, services and networks reload the
 * networks they belong to (collected for a second).
 * The "On" box of a row enables a mux or service at once.
 *
 * The networks come from `mpegts/network/grid` (all of them: the muxes
 * of an IPTV network have no sub-muxes and are a flat list), the muxes
 * of a network from `mpegts/mux/grid` when it is first expanded. The
 * tree is built in `muxTree.ts`. "Reload" loads all again, the network
 * of an edited mux reloads when its editor closes.
 *
 * The flat grid of all muxes is the list view of this tab
 * (DvbMuxesView.vue, route `config-dvb-mux-list`): sortable tuning
 * columns, Hide mode, column picker, phone layout. The toolbar
 * switches to it.
 *
 * Scan: the button of a row queues a scan of its mux (`scan_state` =
 * pending, as the mux editor does). The network is then reloaded every
 * 2 s until these scans are over.
 *
 * Services: expanding a mux shows its services (type, SID, CA systems,
 * channels) - right below it when it has no sub-muxes, else in a
 * "Services (n)" row of their own, collapsed, above the sub-muxes.
 * The services of a network are loaded when the first such row of it is
 * expanded, and again after a scan or an edit.
 */
import {
  computed,
  onBeforeUnmount,
  onMounted,
  ref,
  shallowRef,
  triggerRef,
  watch,
} from 'vue'
import { useRouter } from 'vue-router'
import Button from 'primevue/button'
import {
  ChevronDown,
  ChevronRight,
  List,
  Lock,
  Pencil,
  Plus,
  RefreshCw,
  ScanSearch,
  Trash2,
} from 'lucide-vue-next'
import IdnodeEditor from '@/components/IdnodeEditor.vue'
import IdnodePickEntityDialog from '@/components/IdnodePickEntityDialog.vue'
import SearchInput from '@/components/SearchInput.vue'
import { apiCall } from '@/api/client'
import { cometClient } from '@/api/comet'
import type { IdnodeNotification } from '@/types/comet'
import { useStaleDataRecovery } from '@/composables/useStaleDataRecovery'
import { useAccessStore } from '@/stores/access'
import { useConfirmDialog } from '@/composables/useConfirmDialog'
import { useI18n } from '@/composables/useI18n'
import { useToastNotify } from '@/composables/useToastNotify'
import { apiErrorMessage } from '@/utils/apiErrorMessage'
import {
  buildMuxTree,
  caidList,
  containsMatch,
  countMuxes,
  exactMatch,
  expandableKeys,
  filterTree,
  MUX_FIELDS,
  MUX_TYPE_TS,
  muxSearchText,
  muxTypeName,
  scanEnabled,
  scanQueued,
  scanResultName,
  SCAN_RESULT_FAIL,
  SCAN_STATE_ACTIVE,
  SCAN_STATE_IPEND,
  SCAN_STATE_PEND,
  SERVICE_FIELDS,
  servicesByMux,
  serviceTypeName,
  subMuxScan,
  subtreeCounts,
  tuningLabel,
  type MuxRow,
  type MuxTreeNode,
  type ServiceRow,
  type TreeFilterResult,
} from './muxTree'

const { t } = useI18n()
const access = useAccessStore()
const confirmDialog = useConfirmDialog()
const router = useRouter()
const toast = useToastNotify()

interface NetworkEntry {
  uuid: string
  name: string
  /* a disabled network scans none of its muxes */
  enabled: boolean
  /* its name in the rows of its muxes and services (`mn_display_name`:
   * "unknown" when it has none) */
  serverName: string
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
  /* the "Services (n)" row of a mux, and a service */
  svcGroup?: { mux: MuxRow; count: number }
  service?: ServiceRow
  /* the network of a mux or service row */
  net?: NetworkEntry
  /* a "Loading…" row of services: the network they are loaded for */
  servicesOf?: NetworkEntry
}

const networks = ref<NetworkEntry[]>([])
/* What is loaded is replaced as a whole, shallow: thousands of rows are
 * not made reactive one by one */
const trees = shallowRef<Record<string, MuxTreeNode[]>>({})
const loadingNets = ref(new Set<string>())
const expanded = ref(new Set<string>())
/* per network: its services by mux uuid (loaded when needed) */
const services = shallowRef<Record<string, Map<string, ServiceRow[]>>>({})
const onlyWithSubMuxes = ref(false)
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
        enabled: r.enabled !== false,
        serverName: r.networkname ? String(r.networkname) : 'unknown',
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

/* the services of a network, by mux. A service row names its network,
 * it has no uuid of it: the server filters by the name, then the muxes
 * of the network keep its own services (the filter ignores case, and
 * two networks may have the same name). */
const loadServices = serialized(fetchServices, (net: NetworkEntry) => net.uuid)

async function fetchServices(net: NetworkEntry) {
  try {
    const grid = await apiCall<GridResponse<ServiceRow>>('mpegts/service/grid', {
      start: 0,
      limit: 100000,
      hidemode: 'none',
      list: SERVICE_FIELDS,
      filter: [{ type: 'string', field: 'network', value: exactMatch(net.serverName) }],
    })
    const own = (grid.entries ?? []).filter((r) => {
      const mux = r.multiplex_uuid ? index.value.get(r.multiplex_uuid) : undefined
      return mux?.kind === 'mux' ? mux.net.uuid === net.uuid : r.network === net.serverName
    })
    services.value = { ...services.value, [net.uuid]: servicesByMux(own) }
    setLoadError(`services:${net.uuid}`)
  } catch (e) {
    setLoadError(`services:${net.uuid}`, e)
  }
}

/* once loaded, the services follow scans and edits */
function refreshServices(net: NetworkEntry) {
  if (services.value[net.uuid]) void loadServices(net)
}

function serviceTitle(s: ServiceRow): string {
  const parts = [`SID ${s.sid}`]
  if (s.encrypted) parts.push(t('encrypted: {0}', s.caid ?? ''))
  return parts.join(' — ')
}

/* a copy of the set with the key in it or not (the sets are replaced, so
 * their refs trigger) */
function withKey(set: Set<string>, key: string, on: boolean): Set<string> {
  const next = new Set(set)
  if (on) next.add(key)
  else next.delete(key)
  return next
}

/* the rows a filter opened that were closed by hand: they stay closed
 * until the filter changes */
const filterClosed = ref(new Set<string>())

function setOpen(key: string, open: boolean) {
  expanded.value = withKey(expanded.value, key, open)
  if (filterOn.value) filterClosed.value = withKey(filterClosed.value, key, !open)
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
  /* opening shows services: a "Services (n)" row, a mux without sub-muxes */
  const showsServices = !!row.svcGroup || (!!row.node && !row.node.children.length)
  if (showsServices && !row.open && row.net && !services.value[row.net.uuid]) void loadServices(row.net)
}

async function expandAll() {
  filterClosed.value = new Set()
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
  /* what a filter opened too */
  if (filterOn.value) filterClosed.value = new Set(visible.value.filter((r) => r.hasChildren).map((r) => r.key))
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
  services.value = keep(services.value)
  const svcNets = new Map(servicesWanted.value)
  for (const n of networks.value) if (services.value[n.uuid]) svcNets.set(n.uuid, n)
  const wanted = (n: NetworkEntry) =>
    !!trees.value[n.uuid] || (n.numMux > 0 && (expanded.value.has(n.uuid) || filterOn.value))
  await Promise.all([
    ...networks.value.filter(wanted).map((n) => loadMuxes(n, false)),
    ...[...svcNets.values()].map((n) => loadServices(n)),
    searchServiceNames(),
  ])
}

/* the top nodes shown: with "only with sub-muxes" the transponders
 * that carry some, and the sub-muxes without their transponder */
function topNodes(tree: MuxTreeNode[]): MuxTreeNode[] {
  if (!onlyWithSubMuxes.value) return tree
  return tree.filter((n) => n.children.length > 0 || n.orphan)
}

/* Filter: text (muxes, optionally service names), state */
const filterText = ref('')
const filterQuery = ref('')
let filterTimer: number | undefined
watch(filterText, (v) => {
  window.clearTimeout(filterTimer)
  filterTimer = window.setTimeout(() => (filterQuery.value = v.trim().toLowerCase()), 250)
})
const stateFilter = ref<'all' | 'failed' | 'disabled'>('all')
const searchServices = ref(false)
const filterOn = computed(() => filterQuery.value !== '' || stateFilter.value !== 'all')
onBeforeUnmount(() => window.clearTimeout(filterTimer))
/* another filter: what it opens is open again */
watch([filterQuery, stateFilter, searchServices, onlyWithSubMuxes], () => (filterClosed.value = new Set()))

function muxMatches(n: MuxTreeNode): boolean {
  if (filterQuery.value && !muxSearchText(n).includes(filterQuery.value)) return false
  if (stateFilter.value === 'all') return true
  return stateFilter.value === 'failed' ? n.row.scan_result === SCAN_RESULT_FAIL : n.row.enabled !== 1
}

/* Service names: one search on the server for the text (anywhere in the
 * name, ignoring case, as the mux filter), the first SERVICE_HITS_MAX
 * services found by name - not the services of every network */
const SERVICE_HITS_MAX = 500
const hitsOn = computed(() => searchServices.value && filterQuery.value !== '')
/* the services found, by mux uuid */
const serviceHits = shallowRef(new Map<string, ServiceRow[]>())
const serviceHitsShown = ref(0)
const serviceHitsTotal = ref(0)

const searchServiceNames = serialized<void>(fetchServiceHits, () => 'search')

async function fetchServiceHits() {
  if (!hitsOn.value) {
    serviceHits.value = new Map()
    serviceHitsShown.value = serviceHitsTotal.value = 0
    setLoadError('search')
    return
  }
  try {
    const grid = await apiCall<GridResponse<ServiceRow> & { total?: number }>('mpegts/service/grid', {
      start: 0,
      limit: SERVICE_HITS_MAX,
      hidemode: 'none',
      sort: 'svcname',
      dir: 'ASC',
      list: SERVICE_FIELDS,
      filter: [{ type: 'string', field: 'svcname', value: containsMatch(filterQuery.value) }],
    })
    const rows = grid.entries ?? []
    serviceHits.value = servicesByMux(rows)
    serviceHitsShown.value = rows.length
    serviceHitsTotal.value = Math.max(grid.total ?? 0, rows.length)
    setLoadError('search')
  } catch (e) {
    setLoadError('search', e)
  }
}

const filterResults = computed<Record<string, TreeFilterResult>>(() => {
  const out: Record<string, TreeFilterResult> = {}
  if (!filterOn.value) return out
  const hits = hitsOn.value ? serviceHits.value : null
  for (const net of networks.value) {
    out[net.uuid] = filterTree(topNodes(trees.value[net.uuid] ?? []), muxMatches, (n) =>
      !!hits && (hits.get(n.row.uuid)?.length ?? 0) > 0,
    )
  }
  return out
})

/* the filter needs all networks (the services found hang below their
 * muxes) */
watch(filterOn, (on) => {
  if (!on) return
  for (const n of networks.value) if (n.numMux > 0 && !trees.value[n.uuid]) void loadMuxes(n, false)
})
watch([searchServices, filterQuery], () => void searchServiceNames())

const visible = computed<FlatRow[]>(() => {
  const out: FlatRow[] = []
  /* the services of a mux (loading: a row that says so); a filter on
   * service names shows only the ones it found (hits) */
  const serviceList = (net: NetworkEntry, row: MuxRow, depth: number, hits?: ServiceRow[]) => {
    if (hits) {
      for (const svc of hits) out.push({ key: svc.uuid, depth, service: svc, net, hasChildren: false, open: false })
      return
    }
    const list = services.value[net.uuid]
    if (!list) {
      out.push({
        key: `${row.uuid}:services:loading`,
        depth,
        hasChildren: false,
        open: false,
        loading: true,
        servicesOf: net,
      })
      return
    }
    for (const svc of list.get(row.uuid) ?? [])
      out.push({ key: svc.uuid, depth, service: svc, net, hasChildren: false, open: false })
  }
  /* a mux with sub-muxes: its services in a "Services (n)" row */
  const serviceGroup = (net: NetworkEntry, row: MuxRow, depth: number, hits?: ServiceRow[]) => {
    const key = `${row.uuid}:services`
    const open = (!!hits && !filterClosed.value.has(key)) || expanded.value.has(key)
    out.push({ key, depth, svcGroup: { mux: row, count: row.num_svc ?? 0 }, net, hasChildren: true, open })
    if (open) serviceList(net, row, depth + 1, hits)
  }
  const walk = (net: NetworkEntry, nodes: MuxTreeNode[], depth: number, f: TreeFilterResult | null, inMatch: boolean) => {
    for (const node of nodes) {
      if (f && !inMatch && !f.show.has(node.key)) continue
      const forced = !!f && !inMatch && f.open.has(node.key) && !filterClosed.value.has(node.key)
      const open = forced || expanded.value.has(node.key)
      const wide = depth === 1 && !node.orphan && (node.row.tstype ?? MUX_TYPE_TS) === MUX_TYPE_TS
      const svc = (node.row.num_svc ?? 0) > 0
      out.push({ key: node.key, depth, node, net, hasChildren: node.children.length > 0 || svc, open, wide })
      if (!open) continue
      const self = !!f && f.self.has(node.key)
      /* opened for a service match: only the services found */
      const onlyHits = !!f && !inMatch && !self && hitsOn.value
      if (svc) {
        const hits = onlyHits ? serviceHits.value.get(node.row.uuid) : undefined
        if (!onlyHits || hits?.length) {
          if (node.children.length) serviceGroup(net, node.row, depth + 1, hits)
          else serviceList(net, node.row, depth + 1, hits)
        }
      }
      walk(net, node.children, depth + 1, f, inMatch || self)
    }
  }
  for (const net of networks.value) {
    const f = filterOn.value ? filterResults.value[net.uuid] : null
    /* a filter shows the networks with a match (and those still loading) */
    if (f && (net.numMux === 0 || (trees.value[net.uuid] && f.show.size === 0))) continue
    const open = (!!f && !filterClosed.value.has(net.uuid)) || expanded.value.has(net.uuid)
    out.push({ key: net.uuid, depth: 0, network: net, hasChildren: net.numMux > 0, open, wide: true })
    if (!open) continue
    if (loadingNets.value.has(net.uuid) && !trees.value[net.uuid])
      out.push({ key: `${net.uuid}:loading`, depth: 1, hasChildren: false, open: false, loading: true })
    walk(net, topNodes(trees.value[net.uuid] ?? []), 1, f, false)
  }
  return out
})

/* the networks whose services rows show "Loading…": whatever opened them,
 * their services load (again after a failed load, by "Reload") */
const servicesWanted = computed(() => {
  const nets = new Map<string, NetworkEntry>()
  for (const r of visible.value) if (r.servicesOf) nets.set(r.servicesOf.uuid, r.servicesOf)
  return nets
})
watch(
  () => [...servicesWanted.value.keys()].join(' '),
  () => {
    for (const net of servicesWanted.value.values()) void loadServices(net)
  },
)

const muxTotal = computed(() =>
  Object.values(trees.value).reduce((n, tree) => n + countMuxes(tree), 0),
)

/* every loaded mux and service: its kind and network */
type RowKind = 'mux' | 'service'
interface RowEntry {
  kind: RowKind
  net: NetworkEntry
  /* a mux: its row */
  row?: MuxRow
}

const index = computed(() => {
  const map = new Map<string, RowEntry>()
  for (const net of networks.value) {
    const add = (nodes: MuxTreeNode[]) => {
      for (const n of nodes) {
        map.set(n.row.uuid, { kind: 'mux', net, row: n.row })
        add(n.children)
      }
    }
    add(trees.value[net.uuid] ?? [])
    for (const list of services.value[net.uuid]?.values() ?? [])
      for (const svc of list) map.set(svc.uuid, { kind: 'service', net })
  }
  /* the services found by name: in the network of their mux */
  for (const [mux, list] of serviceHits.value) {
    const e = map.get(mux)
    if (e?.kind === 'mux') for (const svc of list) if (!map.has(svc.uuid)) map.set(svc.uuid, { kind: 'service', net: e.net })
  }
  return map
})

/* Selection: muxes and services, by uuid */
const selected = ref(new Set<string>())
/* the row a Shift click selects from: by uuid, rows above it may have
 * opened or closed since */
let anchor: string | null = null
/* the network of the row clicked last: where Add puts a mux */
const focusNet = ref<NetworkEntry | null>(null)

/* the selection that still exists (a reload may have removed some) */
const selection = computed(() =>
  [...selected.value].filter((u) => index.value.has(u)).map((u) => ({ uuid: u, ...(index.value.get(u) as RowEntry) })),
)

function selectableUuid(r: FlatRow): string | null {
  return r.service?.uuid ?? r.node?.row.uuid ?? null
}

function setSelected(next: Set<string>) {
  selected.value = next
}

function onRowClick(r: FlatRow, ev: MouseEvent) {
  if (r.network) {
    focusNet.value = r.network
    return
  }
  const uuid = selectableUuid(r)
  if (!uuid) return
  focusNet.value = r.net ?? null
  const from = anchor && ev.shiftKey ? visible.value.findIndex((x) => selectableUuid(x) === anchor) : -1
  if (from >= 0) {
    const i = visible.value.findIndex((x) => x.key === r.key)
    const [a, b] = from < i ? [from, i] : [i, from]
    const next = new Set(ev.ctrlKey || ev.metaKey ? selected.value : [])
    for (const row of visible.value.slice(a, b + 1)) {
      const u = selectableUuid(row)
      if (u) next.add(u)
    }
    setSelected(next)
    return
  }
  anchor = uuid
  if (ev.ctrlKey || ev.metaKey) toggleSelected(uuid)
  else setSelected(new Set([uuid]))
}

function toggleSelected(uuid: string) {
  const next = new Set(selected.value)
  if (next.has(uuid)) next.delete(uuid)
  else next.add(uuid)
  setSelected(next)
}

const visibleSelectable = computed(() => visible.value.map(selectableUuid).filter((u): u is string => !!u))
const allSelected = computed(
  () => visibleSelectable.value.length > 0 && visibleSelectable.value.every((u) => selected.value.has(u)),
)

function toggleAll() {
  setSelected(allSelected.value ? new Set() : new Set(visibleSelectable.value))
}

/* Editor: edit (one or several of a kind), create (a mux in a network) */
const editingUuid = ref<string | null>(null)
const editingUuids = ref<string[] | null>(null)
const editingTitle = ref('')
const creatingNet = ref<NetworkEntry | null>(null)
/* the networks that reload when the editor closes */
let editingNets: NetworkEntry[] = []

const createScope = computed(() =>
  creatingNet.value
    ? {
        classEndpoint: 'mpegts/network/mux_class',
        createEndpoint: 'mpegts/network/mux_create',
        params: { uuid: creatingNet.value.uuid },
      }
    : undefined,
)

function networkOf(row: MuxRow): NetworkEntry | null {
  return networks.value.find((n) => n.uuid === row.network_uuid) ?? null
}

function onRowDblclick(row: FlatRow) {
  if (row.service) {
    openEdit([row.service.uuid], 'service', row.net ? [row.net] : [])
  } else if (row.network) {
    editingUuid.value = row.network.uuid
    editingTitle.value = t('Edit Network')
    editingNets = []
  } else if (row.node) {
    const net = networkOf(row.node.row)
    openEdit([row.node.row.uuid], 'mux', net ? [net] : [])
  }
}

function openEdit(uuids: string[], kind: RowKind, nets: NetworkEntry[]) {
  editingNets = nets
  if (uuids.length === 1) {
    editingUuid.value = uuids[0]
    editingTitle.value = kind === 'mux' ? t('Edit Mux') : t('Edit Service')
  } else {
    editingUuids.value = uuids
    editingTitle.value = kind === 'mux' ? t('Edit {0} muxes', uuids.length) : t('Edit {0} services', uuids.length)
  }
}

/* edit: one kind only (a mux and a service have different fields) */
const canEdit = computed(() => {
  const kinds = new Set(selection.value.map((x) => x.kind))
  return kinds.size === 1
})

function onEdit() {
  if (!canEdit.value) return
  const sel = selection.value
  openEdit(
    sel.map((x) => x.uuid),
    sel[0].kind,
    [...new Set(sel.map((x) => x.net))],
  )
}

/* add: to the network of the selection (or the network row clicked),
 * else to one picked */
const pickerVisible = ref(false)

function onAdd() {
  const net = focusNet.value ?? selection.value[0]?.net ?? null
  if (net) openCreate(net)
  else pickerVisible.value = true
}

function onNetworkPicked(uuid: string, name: string) {
  pickerVisible.value = false
  openCreate(
    networks.value.find((n) => n.uuid === uuid) ?? {
      uuid,
      name,
      enabled: true,
      serverName: name,
      numMux: 0,
      numSvc: 0,
      numChn: 0,
    },
  )
}

function openCreate(net: NetworkEntry) {
  creatingNet.value = net
  editingTitle.value = t('Add Mux to {0}', net.name)
  editingNets = [net]
}

/* the new mux is saved: the editor goes on with it */
function onCreated(uuid: string) {
  creatingNet.value = null
  editingUuid.value = uuid
  editingTitle.value = t('Edit Mux')
}

function onEditorClose() {
  editingUuid.value = null
  editingUuids.value = null
  creatingNet.value = null
  if (editingNets.length) {
    for (const net of editingNets) {
      void loadMuxes(net, false)
      refreshServices(net)
    }
  } else void loadNetworks()
  editingNets = []
}

async function onDelete() {
  const sel = selection.value
  if (!sel.length) return
  const muxes = sel.filter((x) => x.kind === 'mux').length
  const svcs = sel.length - muxes
  const what = [muxes ? t('{0} mux(es)', muxes) : '', svcs ? t('{0} service(s)', svcs) : '']
    .filter(Boolean)
    .join(t(' and '))
  const ok = await confirmDialog.ask(t('Do you really want to delete {0}?', what), { severity: 'danger' })
  if (!ok) return
  try {
    await apiCall('idnode/delete', { uuid: JSON.stringify(sel.map((x) => x.uuid)) })
  } catch (e) {
    toast.error(`${t('Failed to delete')}: ${apiErrorMessage(e)}`)
  }
  setSelected(new Set())
  for (const net of new Set(sel.map((x) => x.net))) {
    void loadMuxes(net, false)
    refreshServices(net)
  }
}

/* scan: the muxes of the selection that a scan does something for */
const selectedMuxes = computed(() =>
  selection.value.filter((x) => x.kind === 'mux' && x.net.enabled && !!x.row && scanEnabled(x.row)),
)

function onScanSelection() {
  const byNet = new Map<NetworkEntry, string[]>()
  for (const x of selectedMuxes.value) byNet.set(x.net, [...(byNet.get(x.net) ?? []), x.uuid])
  void scanMuxes(byNet)
}

/* Scans started here: their muxes per network, and the reload timers */
const SCAN_POLL_MS = 2000
const SCAN_POLL_MAX = 300 /* 10 min */
const scanning = new Map<string, Set<string>>()
const scanPolls = new Map<string, number>()

/* the next reload is timed when one is done: no pile-up on a slow server */
function pollScans(net: NetworkEntry) {
  if (scanPolls.has(net.uuid)) return
  let rounds = 0
  const tick = async () => {
    await loadMuxes(net, false)
    /* stopped meanwhile: the view is gone */
    if (!scanPolls.has(net.uuid)) return
    const uuids = scanning.get(net.uuid) ?? new Set<string>()
    /* the idle queue does not count: a mux rotates there for good */
    const busy = (nodes: MuxTreeNode[]): boolean =>
      nodes.some((n) => (uuids.has(n.row.uuid) && scanQueued(n.row)) || busy(n.children))
    if (++rounds >= SCAN_POLL_MAX || !busy(trees.value[net.uuid] ?? [])) {
      scanPolls.delete(net.uuid)
      scanning.delete(net.uuid)
      refreshServices(net)
      return
    }
    scanPolls.set(net.uuid, window.setTimeout(tick, SCAN_POLL_MS))
  }
  scanPolls.set(net.uuid, window.setTimeout(tick, SCAN_POLL_MS))
}

async function scanRow(r: FlatRow) {
  if (!r.node || !r.net) return
  await scanMuxes(new Map([[r.net, [r.node.row.uuid]]]))
}

/* Queue the scans of these muxes (by network): one save for all of them
 * (`scan_state` for a list of uuids, as the mux editor sets it), then
 * each network reloads until its scans are over - after a failed save
 * too, it may have queued some */
async function scanMuxes(byNet: Map<NetworkEntry, string[]>) {
  const uuids = [...byNet.values()].flat()
  if (!uuids.length) return
  try {
    await apiCall('idnode/save', { node: JSON.stringify({ uuid: uuids, scan_state: SCAN_STATE_PEND }) })
  } catch (e) {
    toast.error(`${t('Failed to start scan')}: ${apiErrorMessage(e)}`)
  }
  for (const [net, list] of byNet) {
    const set = scanning.get(net.uuid) ?? new Set<string>()
    for (const u of list) set.add(u)
    scanning.set(net.uuid, set)
  }
  await Promise.all([...byNet.keys()].map((net) => loadMuxes(net, false)))
  for (const net of byNet.keys()) pollScans(net)
}

/* why the scan button of a row does nothing, '' when it does */
function scanBlocked(r: FlatRow): string {
  if (!r.node) return ''
  if (r.net && !r.net.enabled) return t('The network is disabled: it scans no mux')
  if (!scanEnabled(r.node.row)) return t('The mux is not enabled: a scan would be ignored')
  if (scanQueued(r.node.row)) return t('Queued or scanning')
  return ''
}

onBeforeUnmount(() => {
  for (const id of scanPolls.values()) window.clearTimeout(id)
  scanPolls.clear()
})

/* Live updates: the networks whose muxes or services changed reload */
const pendingMux = new Set<string>()
const pendingSvc = new Set<string>()
let pendingMuxCreate = false
let pendingSvcCreate = false
let pendingNetworks = false
let cometTimer: number | undefined

function onNotification(kind: 'mux' | 'service' | 'network', msg: IdnodeNotification) {
  const uuids = [...(msg.change ?? []), ...(msg.delete ?? [])]
  const created = (msg.create ?? []).length > 0
  if (!uuids.length && !created) return
  if (kind === 'network') pendingNetworks = true
  else if (kind === 'mux') {
    for (const u of uuids) pendingMux.add(u)
    if (created) pendingMuxCreate = true
  } else {
    for (const u of uuids) pendingSvc.add(u)
    if (created) pendingSvcCreate = true
  }
  if (cometTimer === undefined) cometTimer = window.setTimeout(applyNotifications, 1000)
}

async function applyNotifications() {
  cometTimer = undefined
  const muxNets = new Set<NetworkEntry>()
  const svcNets = new Set<NetworkEntry>()
  for (const u of pendingMux) {
    const e = index.value.get(u)
    if (e?.kind === 'mux') muxNets.add(e.net)
  }
  /* a service found by name only: the search runs again */
  const hit = new Set([...serviceHits.value.values()].flat().map((x) => x.uuid))
  let search = pendingSvcCreate
  for (const u of pendingSvc) {
    const e = index.value.get(u)
    if (e?.kind === 'service' && services.value[e.net.uuid]) svcNets.add(e.net)
    if (hit.has(u)) search = true
  }
  /* a new one: its network is not known here, the loaded ones reload */
  if (pendingMuxCreate) for (const n of networks.value) if (trees.value[n.uuid]) muxNets.add(n)
  if (pendingSvcCreate) for (const n of networks.value) if (services.value[n.uuid]) svcNets.add(n)
  const nets = pendingNetworks
  pendingMux.clear()
  pendingSvc.clear()
  pendingMuxCreate = pendingSvcCreate = pendingNetworks = false
  if (nets) await loadNetworks()
  for (const n of muxNets) void loadMuxes(n, false)
  for (const n of svcNets) void loadServices(n)
  if (search && hitsOn.value) void searchServiceNames()
}

const cometOff = [
  cometClient.on('mpegts_mux', (m) => onNotification('mux', m as IdnodeNotification)),
  cometClient.on('service', (m) => onNotification('service', m as IdnodeNotification)),
  cometClient.on('mpegts_network', (m) => onNotification('network', m as IdnodeNotification)),
]
onBeforeUnmount(() => {
  for (const off of cometOff) off()
  window.clearTimeout(cometTimer)
})
/* after a long Comet gap (tab asleep, server restarted): all again */
useStaleDataRecovery({ refetch: () => void reload() })

/* In the rows: enable. The change shows at once; when its save fails,
 * the old value is back and the network reloads. */
async function saveFields(uuid: string, fields: Record<string, unknown>): Promise<boolean> {
  try {
    await apiCall('idnode/save', { node: JSON.stringify({ uuid, ...fields }) })
    return true
  } catch (e) {
    toast.error(`${t('Failed to save')}: ${apiErrorMessage(e)}`)
    return false
  }
}

/* the muxes of the network of a row as the server has them */
function reloadNetworkOf(row: MuxRow) {
  const net = networkOf(row)
  if (net) void loadMuxes(net, false)
}

/* "On": a mux 1/0 (-1 = ignore: half set, a click enables it), a service */
function enabledState(r: FlatRow): 'on' | 'off' | 'ignore' | null {
  if (r.service) return r.service.enabled === false ? 'off' : 'on'
  const row = r.node?.row
  if (!row) return null
  return row.enabled === 1 ? 'on' : row.enabled === -1 ? 'ignore' : 'off'
}

async function toggleEnabled(r: FlatRow) {
  if (r.service) {
    const svc = r.service
    const old = svc.enabled
    svc.enabled = old === false
    triggerRef(services)
    triggerRef(serviceHits)
    if (await saveFields(svc.uuid, { enabled: svc.enabled })) return
    svc.enabled = old
    triggerRef(services)
    triggerRef(serviceHits)
    if (r.net) refreshServices(r.net)
  } else if (r.node) {
    const row = r.node.row
    const old = row.enabled
    row.enabled = old === 1 ? 0 : 1
    triggerRef(trees)
    if (await saveFields(row.uuid, { enabled: row.enabled })) return
    row.enabled = old
    triggerRef(trees)
    reloadNetworkOf(row)
  }
}

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
      <Button severity="secondary" outlined size="small" :title="t('Add a mux to the network of the selection')" @click="onAdd">
        <Plus :size="14" :stroke-width="2" />
        {{ t('Add') }}
      </Button>
      <Button severity="secondary" outlined size="small" :disabled="!canEdit" @click="onEdit">
        <Pencil :size="14" :stroke-width="2" />
        {{ t('Edit') }}
      </Button>
      <Button severity="secondary" outlined size="small" :disabled="!selection.length" @click="onDelete">
        <Trash2 :size="14" :stroke-width="2" />
        {{ t('Delete') }}
      </Button>
      <Button severity="secondary" outlined size="small" :disabled="!selectedMuxes.length" @click="onScanSelection">
        <ScanSearch :size="14" :stroke-width="2" />
        {{ t('Scan') }}
      </Button>
      <span class="mux-tree__sep" />
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
        <template v-if="selection.length">{{ t('{0} selected', selection.length) }} · </template>
        {{ t('{0} networks, {1} muxes loaded', networks.length, muxTotal) }}
      </span>
    </div>
    <div class="mux-tree__toolbar mux-tree__filters">
      <SearchInput
        v-model="filterText"
        class="mux-tree__search"
        :placeholder="t('Filter: name, type, provider, tuning')"
        :aria-label="t('Filter')"
      />
      <label class="mux-tree__check">
        <input v-model="searchServices" type="checkbox" />
        {{ t('Also service names') }}
      </label>
      <select v-model="stateFilter" class="mux-tree__select" :aria-label="t('State')">
        <option value="all">{{ t('All muxes') }}</option>
        <option value="failed">{{ t('Scan failed') }}</option>
        <option value="disabled">{{ t('Disabled') }}</option>
      </select>
      <label class="mux-tree__check">
        <input v-model="onlyWithSubMuxes" type="checkbox" />
        {{ t('Only transponders with sub-muxes') }}
      </label>
      <span v-if="hitsOn && serviceHitsTotal > serviceHitsShown" class="mux-tree__hits" role="status">
        {{ t('The first {0} of {1} services found: refine the filter', serviceHitsShown, serviceHitsTotal) }}
      </span>
    </div>
    <p v-if="loadError" class="mux-tree__error" role="alert">{{ t('Failed to load:') }} {{ loadError }}</p>
    <div class="mux-tree__scroll">
      <table class="mux-tree__table" :aria-label="t('Muxes')">
        <colgroup>
          <col class="mux-tree__col-select" />
          <col />
          <col class="mux-tree__col-type" />
          <col class="mux-tree__col-sid" />
          <col class="mux-tree__col-scan" />
          <col class="mux-tree__col-count" />
          <col class="mux-tree__col-count" />
          <col class="mux-tree__col-on" />
          <col class="mux-tree__col-action" />
        </colgroup>
        <thead>
          <tr>
            <th class="mux-tree__select-cell">
              <input
                type="checkbox"
                :checked="allSelected"
                :aria-label="t('Select all shown')"
                @change="toggleAll"
              />
            </th>
            <th class="mux-tree__name">{{ t('Name') }}</th>
            <th>{{ t('Type') }}</th>
            <th>{{ t('SID') }}</th>
            <th>{{ t('Scan result') }}</th>
            <th class="mux-tree__num">{{ t('Services') }}</th>
            <th class="mux-tree__num">{{ t('Channels') }}</th>
            <th class="mux-tree__select-cell" :title="t('Enabled')">{{ t('On') }}</th>
            <th><span class="mux-tree__sr-only">{{ t('Scan') }}</span></th>
          </tr>
        </thead>
        <tbody>
          <tr
            v-for="r in visible"
            :key="r.key"
            class="mux-tree__row"
            :class="{
              'mux-tree__row--selected': !!selectableUuid(r) && selected.has(selectableUuid(r) as string),
              'mux-tree__row--network': !!r.network,
              'mux-tree__row--off': (!!r.node && r.node.row.enabled !== 1) || r.service?.enabled === false,
              'mux-tree__row--group': !!r.svcGroup,
            }"
            @click="onRowClick(r, $event)"
            @dblclick="onRowDblclick(r)"
          >
            <td class="mux-tree__select-cell">
              <input
                v-if="selectableUuid(r)"
                type="checkbox"
                :checked="selected.has(selectableUuid(r) as string)"
                :aria-label="t('Select')"
                @click.stop="toggleSelected(selectableUuid(r) as string)"
                @dblclick.stop
              />
            </td>
            <!-- network and transponder rows: the name uses the columns before the scan result -->
            <td
              class="mux-tree__name"
              :colspan="r.wide ? 3 : undefined"
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
              <template v-else-if="r.svcGroup">
                {{ t('Services') }}
                <span class="mux-tree__muted">({{ r.svcGroup.count }})</span>
              </template>
              <template v-else-if="r.service">
                <template v-if="r.service.svcname">{{ r.service.svcname }}</template>
                <span v-else class="mux-tree__muted">–</span>
                <span v-if="r.service.enabled === false" class="mux-tree__badge">{{ t('disabled') }}</span>
                <span v-if="r.service.provider" class="mux-tree__provider">{{ r.service.provider }}</span>
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
              <template v-if="!r.wide">
                <td>{{ muxTypeName(r.node.row.tstype) }}</td>
                <td />
              </template>
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
            <template v-else-if="r.service">
              <td>{{ serviceTypeName(r.service.dvb_servicetype) }}</td>
              <td class="mux-tree__sid" :title="serviceTitle(r.service)">
                <Lock v-if="r.service.encrypted" class="mux-tree__lock" :size="12" :stroke-width="2" />
                {{ r.service.sid }}<template v-if="r.service.encrypted"> · {{ caidList(r.service.caid) }}</template>
              </td>
              <td colspan="2" class="mux-tree__streams" :title="r.service.streams">{{ r.service.streams }}</td>
              <td class="mux-tree__num">{{ r.service.channel?.length || '' }}</td>
            </template>
            <template v-else-if="r.network">
              <td>
              </td>
              <td class="mux-tree__num">{{ r.network.numSvc }}</td>
              <td class="mux-tree__num">{{ r.network.numChn }}</td>
            </template>
            <td v-else colspan="5" />
            <td class="mux-tree__select-cell">
              <input
                v-if="enabledState(r)"
                type="checkbox"
                :checked="enabledState(r) === 'on'"
                :indeterminate="enabledState(r) === 'ignore'"
                :title="enabledState(r) === 'ignore' ? t('Ignored: a click enables it') : t('Enabled')"
                :aria-label="t('Enabled')"
                @click.stop="toggleEnabled(r)"
                @dblclick.stop
              />
            </td>
            <td class="mux-tree__action">
              <button
                v-if="r.node"
                type="button"
                class="mux-tree__scan"
                :disabled="!!scanBlocked(r)"
                :title="scanBlocked(r) || t('Scan this mux')"
                :aria-label="t('Scan')"
                @click.stop="scanRow(r)"
                @dblclick.stop
              >
                <ScanSearch :size="15" :stroke-width="2" />
              </button>
            </td>
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
  <IdnodePickEntityDialog
    :visible="pickerVisible"
    grid-endpoint="mpegts/network/grid"
    display-field="networkname"
    :title="t('Add Mux')"
    :label="t('Network')"
    @pick="onNetworkPicked"
    @close="pickerVisible = false"
  />
  <IdnodeEditor
    :uuid="editingUuid"
    :uuids="editingUuids"
    :create-base="creatingNet ? 'mpegts/network' : null"
    :parent-scoped="createScope"
    :level="access.uilevel"
    :title="editingTitle"
    @close="onEditorClose"
    @created="onCreated"
  />
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

.mux-tree__check {
  display: inline-flex;
  align-items: center;
  gap: var(--tvh-space-1);
  font-size: var(--tvh-text-sm);
  margin-left: var(--tvh-space-2);
}

.mux-tree__hits {
  color: var(--tvh-warning);
  font-size: var(--tvh-text-sm);
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

.mux-tree__col-sid {
  width: 148px;
}

.mux-tree__col-scan {
  width: 128px;
}

.mux-tree__col-count {
  width: 78px;
}

.mux-tree__col-action {
  width: 64px;
}

.mux-tree__col-select {
  width: 30px;
}

.mux-tree__col-on {
  width: 34px;
}

.mux-tree__select-cell {
  text-align: center;
  padding: 0 !important;
}

.mux-tree__row--selected td {
  background: color-mix(in srgb, var(--tvh-primary) 14%, transparent);
}

.mux-tree__streams {
  color: var(--tvh-text-muted);
  font-size: var(--tvh-text-xs);
}

.mux-tree__sep {
  width: 1px;
  height: 1.4rem;
  background: var(--tvh-border);
  margin: 0 var(--tvh-space-1);
}

.mux-tree__filters {
  padding-top: 0;
}

.mux-tree__search {
  width: 22rem;
  max-width: 100%;
}

.mux-tree__select {
  font: inherit;
  font-size: var(--tvh-text-sm);
  padding: 2px var(--tvh-space-1);
  border: 1px solid var(--tvh-border);
  border-radius: var(--tvh-radius-sm);
  background: var(--tvh-bg-surface);
  color: var(--tvh-text);
}

.mux-tree__action {
  text-align: center;
}

.mux-tree__scan {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  width: 1.6rem;
  height: 1.6rem;
  padding: 0;
  border: none;
  border-radius: var(--tvh-radius-sm);
  background: none;
  color: var(--tvh-text-muted);
  cursor: pointer;
}

.mux-tree__scan:hover:not(:disabled) {
  color: var(--tvh-primary);
  background: color-mix(in srgb, var(--tvh-primary) var(--tvh-hover-strength), transparent);
}

.mux-tree__scan:disabled {
  opacity: 0.35;
  cursor: default;
}

.mux-tree__scan:focus-visible {
  outline: 2px solid var(--tvh-focus);
}

.mux-tree__busy {
  color: var(--tvh-primary);
}

.mux-tree__sr-only {
  position: absolute;
  width: 1px;
  height: 1px;
  overflow: hidden;
  clip: rect(0 0 0 0);
  white-space: nowrap;
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

.mux-tree__sid {
  font-variant-numeric: tabular-nums;
}

.mux-tree__lock {
  vertical-align: -1px;
  margin-right: 2px;
  color: var(--tvh-warning);
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

.mux-tree__row--group td {
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
