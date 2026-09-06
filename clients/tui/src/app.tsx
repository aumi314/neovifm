import { createEffect, createMemo, createSignal, For, onCleanup, Show } from "solid-js"
import { useKeyboard, useRenderer, useTerminalDimensions } from "@opentui/solid"
import { MouseButton, SyntaxStyle, type ScrollBoxRenderable } from "@opentui/core"

import type {
  PaneId,
  PaneSortKey,
  ActionTaskPayload,
  PreviewPayload,
  PreviewTaskPayload,
  ResourceTaskPayload,
  OpenPayload,
  SnapshotPayload,
  WorkspaceSnapshotPayload,
  PaneTabPayload,
} from "./protocol.js"
import type { CoreActionTarget, CoreSessionCommand } from "./core-client.js"
import { VifmKeymap, type FunctionAction } from "./keymap.js"
import { resolvePutSource, yankFromSnapshot, type YankBuffer } from "./yank.js"
import {
  extensionGroup,
  formatFileSize,
  formatMtime,
  iconForEntry,
  mtimeAge,
  permissionTokens,
  type IconMode,
} from "./file-style.js"
import { formatStatusPath, type StatusPathMode } from "./status-path.js"

const COLORS = {
  crust: "#11111b",
  base: "#1e1e2e",
  mantle: "#181825",
  surface0: "#313244",
  surface1: "#45475a",
  text: "#cdd6f4",
  subtext0: "#a6adc8",
  overlay1: "#7f849c",
  red: "#f38ba8",
  peach: "#fab387",
  yellow: "#f9e2af",
  green: "#a6e3a1",
  sapphire: "#74c7ec",
  lavender: "#b4befe",
  mauve: "#cba6f7",
  teal: "#94e2d5",
  blue: "#89b4fa",
  selected: "#363a4f",
  sortActive: "#585b70",
  divider: "#3f4254",
  permissionType: "#7f849c",
  permissionRead: "#a6e3a1",
  permissionWrite: "#f9e2af",
  permissionExecute: "#f38ba8",
  permissionSticky: "#fab387",
  permissionNone: "#6c7086",
  recentHour: "#f5e0e8",
  recentDay: "#f9e2af",
} as const

export interface AppProps {
  readonly workspace?: WorkspaceSnapshotPayload
  readonly error?: string
  readonly loading?: boolean
  readonly preview?: PreviewPayload
  readonly tasks?: readonly PreviewTaskPayload[]
  readonly actionTasks?: readonly ActionTaskPayload[]
  readonly resourceTasks?: readonly ResourceTaskPayload[]
  readonly commandError?: string
  readonly capabilities?: readonly string[]
  readonly iconMode?: IconMode
  readonly onCancel?: () => void
  readonly onCommand?: (command: CoreSessionCommand) => boolean | Promise<boolean> | void
  readonly onEdit?: (path: string) => Promise<void> | void
  /** Opens a regular file through the core-resolved association or platform fallback. */
  readonly onOpen?: (path: string) => Promise<void> | void
  readonly open?: OpenPayload
  readonly onOpenResolved?: (argv: readonly string[]) => Promise<void> | void
  readonly homeDirectory?: string
  readonly onCopyText?: (text: string) => Promise<void> | void
}

type DialogState =
  | Readonly<{
      kind: "mkdir"
      pane: PaneId
      cwd_bytes_hex: string
      snapshot_revision: string
      cwd_device: string
      cwd_inode: string
      cwd_ctime_unix_ns: string
    }>
  | Readonly<{ kind: "mount-ssh"; pane: PaneId }>
  | Readonly<{ kind: "search"; direction: -1 | 1 }>
  | Readonly<{ kind: "delete"; target: string; command: CoreSessionCommand }>
  | Readonly<{
      kind: "rename"
      rootOnly: boolean
      pane: PaneId
      original: string
      path_bytes_hex: string
      /** Remaining selection paths still queued behind the current one. */
      queue?: readonly string[]
      /** 1-based position and total size of the batch this dialog belongs to. */
      step?: number
      total?: number
    }>

// Splits a basename for Vifm's cW: the extension is the last dot suffix, but a
// leading dot alone (dotfiles) never starts an extension.
function splitFileName(name: string): { root: string; extension: string } {
  const dot = name.lastIndexOf(".")
  if (dot <= 0) return { root: name, extension: "" }
  return { root: name.slice(0, dot), extension: name.slice(dot) }
}

function entryColor(entry: SnapshotPayload["entries"][number]): string {
  if (entry.kind === "directory") return COLORS.blue
  if (entry.kind === "executable") return COLORS.green
  if (entry.kind === "symlink") return COLORS.teal
  const group = extensionGroup(entry)
  if (group === "archive") return COLORS.red
  if (group === "code") return COLORS.green
  if (group === "document") return COLORS.yellow
  if (group === "image") return COLORS.mauve
  if (group === "media") return COLORS.peach
  return COLORS.text
}

function entryTextColor(entry: SnapshotPayload["entries"][number], emphasized: boolean): string {
  if (emphasized) return COLORS.text
  const age = mtimeAge(entry.mtime_unix_ms)
  if (age === "hour") return COLORS.recentHour
  if (age === "day") return COLORS.recentDay
  return entryColor(entry)
}

function permissionColor(kind: ReturnType<typeof permissionTokens>[number]["kind"]): string {
  switch (kind) {
    case "type": return COLORS.permissionType
    case "read": return COLORS.permissionRead
    case "write": return COLORS.permissionWrite
    case "execute": return COLORS.permissionExecute
    case "sticky": return COLORS.permissionSticky
    case "none": return COLORS.permissionNone
    case "unknown": return COLORS.overlay1
  }
}

function sortLabel(snapshot: SnapshotPayload, key: PaneSortKey, label: string): string {
  if (snapshot.sort_key !== key) return label
  return `${label} ${snapshot.sort_descending ? "▼" : "▲"}`
}

function compactMetadataKey(snapshot: SnapshotPayload): "size" | "ctime" | "mtime" | "mode" {
  if (snapshot.sort_key === "ctime" || snapshot.sort_key === "mtime" || snapshot.sort_key === "mode") return snapshot.sort_key
  return "size"
}

function metadataLabel(key: "size" | "ctime" | "mtime" | "mode"): string {
  return key === "size" ? "Size" : key === "ctime" ? "Created" : key === "mtime" ? "Modified" : "Permissions"
}

function metadataWidth(key: "size" | "ctime" | "mtime" | "mode"): number {
  return key === "size" ? 10 : key === "mode" ? 18 : 18
}

function ownerColumnWidth(): number {
  return 12
}

function hasOwnerGroup(snapshot: SnapshotPayload): boolean {
  return snapshot.entries.some((entry) => entry.owner_display !== undefined || entry.group_display !== undefined)
}

function ColumnHeader(props: {
  readonly id: string
  readonly label: string
  readonly active: boolean
  readonly iconMode: IconMode
  readonly width?: number
  readonly grow?: boolean
  readonly onToggle: () => void
  readonly onCycle: () => void
}) {
  return <box
    id={props.id}
    width={props.width}
    height={1}
    flexGrow={props.grow ? 1 : 0}
    flexDirection="row"
    backgroundColor={COLORS.surface0}
    onMouseDown={(event) => {
      event.preventDefault()
      event.stopPropagation()
      if (event.button === MouseButton.RIGHT) props.onCycle()
      else if (event.button === MouseButton.LEFT) props.onToggle()
    }}
  >
    <Show when={props.active && props.iconMode !== "ascii"} fallback={<text fg={props.active ? COLORS.text : COLORS.subtext0}>{props.active ? `[${props.label}]` : ` ${props.label} `}</text>}>
      <text fg={COLORS.sortActive} bg={COLORS.surface0}></text>
      <text fg={COLORS.text} bg={COLORS.sortActive}> {props.label} </text>
      <text fg={COLORS.sortActive} bg={COLORS.surface0}></text>
    </Show>
  </box>
}

function PaneColumns(props: {
  readonly pane: PaneId
  readonly snapshot: SnapshotPayload
  readonly detailed: boolean
  readonly showOwners: boolean
  readonly iconMode: IconMode
  readonly onSortDirection: (pane: PaneId, key: PaneSortKey) => void
  readonly onSortCycle: (pane: PaneId, delta: -1 | 1) => void
}) {
  const header = (key: PaneSortKey, label: string) => sortLabel(props.snapshot, key, label)
  const compactKey = () => compactMetadataKey(props.snapshot)
  return <box width="100%" height={1} flexDirection="row" backgroundColor={COLORS.surface0}>
    <text width={4} bg={COLORS.surface0}> </text>
    <ColumnHeader id={`sort-${props.pane}-name`} label={header("name", "Name")} active={props.snapshot.sort_key === "name"} iconMode={props.iconMode} grow onToggle={() => props.onSortDirection(props.pane, props.snapshot.sort_key)} onCycle={() => props.onSortCycle(props.pane, 1)} />
    <Show when={props.detailed} fallback={<ColumnHeader
      id={`sort-${props.pane}-${compactKey()}`}
      label={header(compactKey(), metadataLabel(compactKey()))}
      active={props.snapshot.sort_key === compactKey()}
      iconMode={props.iconMode}
      width={metadataWidth(compactKey())}
      onToggle={() => props.onSortDirection(props.pane, props.snapshot.sort_key)}
      onCycle={() => props.onSortCycle(props.pane, 1)}
    />}>
      <ColumnHeader id={`sort-${props.pane}-mode`} label={header("mode", "Permissions")} active={props.snapshot.sort_key === "mode"} iconMode={props.iconMode} width={18} onToggle={() => props.onSortDirection(props.pane, props.snapshot.sort_key)} onCycle={() => props.onSortCycle(props.pane, 1)} />
      <Show when={props.showOwners}>
        <ColumnHeader id={`sort-${props.pane}-owner`} label="Owner" active={false} iconMode={props.iconMode} width={ownerColumnWidth()} onToggle={() => undefined} onCycle={() => undefined} />
        <ColumnHeader id={`sort-${props.pane}-group`} label="Group" active={false} iconMode={props.iconMode} width={ownerColumnWidth()} onToggle={() => undefined} onCycle={() => undefined} />
      </Show>
      <ColumnHeader id={`sort-${props.pane}-size`} label={header("size", "Size")} active={props.snapshot.sort_key === "size"} iconMode={props.iconMode} width={10} onToggle={() => props.onSortDirection(props.pane, props.snapshot.sort_key)} onCycle={() => props.onSortCycle(props.pane, 1)} />
      <ColumnHeader id={`sort-${props.pane}-ctime`} label={header("ctime", "Created")} active={props.snapshot.sort_key === "ctime"} iconMode={props.iconMode} width={18} onToggle={() => props.onSortDirection(props.pane, props.snapshot.sort_key)} onCycle={() => props.onSortCycle(props.pane, 1)} />
      <ColumnHeader id={`sort-${props.pane}-mtime`} label={header("mtime", "Modified")} active={props.snapshot.sort_key === "mtime"} iconMode={props.iconMode} width={18} onToggle={() => props.onSortDirection(props.pane, props.snapshot.sort_key)} onCycle={() => props.onSortCycle(props.pane, 1)} />
    </Show>
  </box>
}

function PermissionValue(props: { readonly entry: SnapshotPayload["entries"][number] }) {
  return <box width={18} flexDirection="row" flexShrink={0}>
    <text> </text>
    <For each={permissionTokens(props.entry.mode_octal, props.entry.kind)}>
      {(token) => <text fg={permissionColor(token.kind)}>{token.character}</text>}
    </For>
  </box>
}

function PreviewCopyButton(props: { readonly content?: string; readonly onCopy: (text: string) => void }) {
  const enabled = () => props.content !== undefined
  return <text
    id="preview-copy"
    width={8}
    height={1}
    justifyContent="center"
    bg={enabled() ? COLORS.surface1 : COLORS.mantle}
    fg={enabled() ? COLORS.text : COLORS.overlay1}
    onMouseDown={(event) => {
      if (event.button !== MouseButton.LEFT || props.content === undefined) return
      event.preventDefault()
      event.stopPropagation()
      props.onCopy(props.content)
    }}
  > Copy </text>
}

function PreviewFooter(props: { readonly quick: boolean; readonly content?: string; readonly onCopy: (text: string) => void }) {
  return <box width="100%" height={1} flexDirection="row" alignItems="center">
    <text flexGrow={1} fg={COLORS.subtext0}>{props.quick ? "Space/Esc closes · Tab switches pane" : "F3 or Esc to close"}</text>
    <PreviewCopyButton content={props.content} onCopy={props.onCopy} />
  </box>
}

function QuickPreview(props: { readonly preview?: PreviewPayload; readonly onCopy: (text: string) => void }) {
  const preview = () => props.preview
  return <box flexGrow={1} width="100%" flexDirection="column" backgroundColor={COLORS.base} border borderStyle="rounded" borderColor={COLORS.teal} paddingX={1} title="SPACE QUICK VIEW" titleColor={COLORS.teal}>
    <text height={1} fg={COLORS.subtext0}>{preview() === undefined ? "loading preview" : `${preview()!.kind} · ${preview()!.state}`}</text>
    <scrollbox
      flexGrow={1}
      width="100%"
      backgroundColor={COLORS.base}
      verticalScrollbarOptions={{ showArrows: false, trackOptions: { width: 1, foregroundColor: COLORS.teal, backgroundColor: COLORS.surface0 } }}
    >
      <text id="preview-content" selectable selectionBg={COLORS.selected} selectionFg={COLORS.text} fg={COLORS.text} wrapMode="word">{previewIsLoading(preview()) ? "Loading preview..." : preview()!.content ?? preview()!.error_code ?? "Preview unavailable"}</text>
    </scrollbox>
    <PreviewFooter quick content={preview()?.content} onCopy={props.onCopy} />
  </box>
}

function CompactMetadata(props: {
  readonly entry: SnapshotPayload["entries"][number]
  readonly metadataKey: () => "size" | "ctime" | "mtime" | "mode"
}) {
  return <Show when={props.metadataKey() === "mode"} fallback={<text width={metadataWidth(props.metadataKey())} fg={props.metadataKey() === "mtime" ? entryTextColor(props.entry, props.entry.selected) : COLORS.subtext0}> {props.metadataKey() === "size"
    ? formatFileSize(props.entry.size_bytes)
    : props.metadataKey() === "ctime"
      ? formatMtime((BigInt(props.entry.ctime_unix_ns ?? "0") / 1_000_000n).toString())
      : formatMtime(props.entry.mtime_unix_ms)}</text>}>
    <PermissionValue entry={props.entry} />
  </Show>
}

function EntryList(props: {
  readonly pane: PaneId
  readonly tabId: string
  readonly snapshot: SnapshotPayload
  readonly detailed: boolean
  readonly showOwners: boolean
  readonly iconMode: IconMode
  readonly onSelect: (pane: PaneId, index: number, toggle: boolean) => void
}) {
  const compactKey = () => compactMetadataKey(props.snapshot)
  let list: ScrollBoxRenderable | undefined
  let scrollTimer: ReturnType<typeof setTimeout> | undefined
  let activeLocationKey: string | undefined
  const scrollPositions = new Map<string, number>()
  const rememberScrollPosition = (locationKey: string, position: number) => {
    if (!Number.isFinite(position) || position < 0) return
    scrollPositions.delete(locationKey)
    scrollPositions.set(locationKey, position)
    while (scrollPositions.size > 128) {
      const oldest = scrollPositions.keys().next().value
      if (oldest === undefined) break
      scrollPositions.delete(oldest)
    }
  }
  createEffect(() => {
    const directory = props.snapshot.cwd_bytes_hex
    const locationKey = `${props.tabId}:${directory}`
    const cursor = props.snapshot.cursor
    const directoryChanged = activeLocationKey !== locationKey
    if (directoryChanged && activeLocationKey !== undefined && list !== undefined) {
      rememberScrollPosition(activeLocationKey, list.scrollTop)
    }
    activeLocationKey = locationKey
    const rememberedScrollTop = directoryChanged ? scrollPositions.get(locationKey) : undefined
    if (scrollTimer !== undefined) clearTimeout(scrollTimer)
    scrollTimer = setTimeout(() => {
      if (list === undefined) return
      if (rememberedScrollTop !== undefined) list.scrollTop = rememberedScrollTop
      if (cursor >= 0) list.scrollChildIntoView(`entry-${props.pane}-${cursor}`)
      rememberScrollPosition(locationKey, list.scrollTop)
    }, 0)
    onCleanup(() => {
      if (scrollTimer !== undefined) clearTimeout(scrollTimer)
    })
  })
  return <scrollbox
    id={`entries-${props.pane}`}
    ref={(value) => { list = value }}
    flexGrow={1}
    width="100%"
    backgroundColor={COLORS.base}
    verticalScrollbarOptions={{ showArrows: false, trackOptions: { width: 1, foregroundColor: COLORS.surface1, backgroundColor: COLORS.base } }}
  >
    <For each={props.snapshot.entries}>
      {(entry, index) => {
        const current = () => index() === props.snapshot.cursor
        return <box
          id={`entry-${props.pane}-${index()}`}
          height={1}
          width="100%"
          flexDirection="row"
          backgroundColor={entry.selected || current() ? COLORS.selected : COLORS.base}
          onMouseDown={(event) => {
            if (event.button !== MouseButton.LEFT && event.button !== MouseButton.RIGHT) return
            event.preventDefault()
            event.stopPropagation()
            props.onSelect(props.pane, index(), event.button === MouseButton.RIGHT)
          }}
        >
          <text width={1} fg={current() ? COLORS.lavender : COLORS.overlay1}>{current() ? ">" : " "}</text>
          <text width={1} fg={entry.selected ? COLORS.peach : COLORS.overlay1}>{entry.selected ? "*" : " "}</text>
          <text width={2} fg={entryColor(entry)}>{iconForEntry(entry, props.iconMode)}</text>
          <text flexGrow={1} fg={entry.hidden ? COLORS.overlay1 : entryTextColor(entry, entry.selected || current())} truncate>{entry.name_display}</text>
          <Show when={props.detailed} fallback={<CompactMetadata entry={entry} metadataKey={compactKey} />}>
            <PermissionValue entry={entry} />
            <Show when={props.showOwners}>
              <text width={ownerColumnWidth()} fg={COLORS.subtext0} truncate> {entry.owner_display ?? "-"}</text>
              <text width={ownerColumnWidth()} fg={COLORS.subtext0} truncate> {entry.group_display ?? "-"}</text>
            </Show>
            <text width={10} fg={COLORS.subtext0}> {formatFileSize(entry.size_bytes)}</text>
            <text width={18} fg={COLORS.subtext0}> {formatMtime((BigInt(entry.ctime_unix_ns ?? "0") / 1_000_000n).toString())}</text>
            <text width={18} fg={entryTextColor(entry, entry.selected || current())}> {formatMtime(entry.mtime_unix_ms)}</text>
          </Show>
        </box>
      }}
    </For>
  </scrollbox>
}

function tabLabel(path: string): string {
  const trimmed = path.length > 1 ? path.replace(/[\\/]+$/, "") : path
  const separator = Math.max(trimmed.lastIndexOf("/"), trimmed.lastIndexOf("\\"))
  const label = trimmed.slice(separator + 1)
  return label.length === 0 ? trimmed : label
}

function compactTabLabel(label: string, maximum: number, iconMode: IconMode): string {
  const characters = Array.from(label)
  if (characters.length <= maximum) return label
  return `${characters.slice(0, maximum - 1).join("")}${iconMode === "ascii" ? "~" : "…"}`
}

function PaneTabs(props: {
  readonly pane: PaneId
  readonly tabs: readonly PaneTabPayload[]
  readonly activePane: boolean
  readonly iconMode: IconMode
  readonly enabled: boolean
  readonly onActivate: (pane: PaneId, tabId: string) => void
  readonly onClose: (pane: PaneId, tabId: string) => void
  readonly onNew: (pane: PaneId) => void
}) {
  const marker = () => props.activePane ? (props.iconMode === "ascii" ? "*" : "●") : " "
  return <box width="100%" height={1} flexDirection="row" backgroundColor={COLORS.crust}>
    <For each={props.tabs}>
      {(tab, index) => {
        const color = () => tab.active ? COLORS.lavender : COLORS.surface1
        const body = () => {
          const number = String(index() + 1)
          if (props.tabs.length >= 3 && !tab.active) return number
          const maximum = props.tabs.length === 1 ? 20 : props.tabs.length === 2 ? 9 : 6
          return `${number} ${compactTabLabel(tabLabel(tab.cwd_display), maximum, props.iconMode)}`
        }
        return <box
          id={`tab-${props.pane}-${tab.id}`}
          height={1}
          flexDirection="row"
          onMouseDown={(event) => {
            if (!props.enabled || (event.button !== MouseButton.LEFT && event.button !== MouseButton.RIGHT)) return
            event.preventDefault()
            event.stopPropagation()
            if (event.button === MouseButton.RIGHT) props.onClose(props.pane, tab.id)
            else props.onActivate(props.pane, tab.id)
          }}
        >
          <Show when={props.iconMode !== "ascii"} fallback={<text fg={tab.active ? COLORS.crust : COLORS.text} bg={color()}>[{body()}]</text>}>
            <text fg={color()} bg={COLORS.crust}></text>
            <text fg={tab.active ? COLORS.crust : COLORS.text} bg={color()}>{body()}</text>
            <text fg={color()} bg={COLORS.crust}></text>
          </Show>
        </box>
      }}
    </For>
    <text flexGrow={1}> </text>
    <text id={`active-marker-${props.pane}`} width={2} fg={COLORS.lavender}>{marker()} </text>
    <Show when={props.enabled}>
      <box id={`tab-${props.pane}-new`} width={5} height={1} justifyContent="center" alignItems="center" onMouseDown={(event) => {
        if (event.button !== MouseButton.LEFT) return
        event.preventDefault()
        event.stopPropagation()
        props.onNew(props.pane)
      }}>
        <Show when={props.iconMode !== "ascii"} fallback={<text fg={COLORS.green}>[ + ]</text>}>
          <text fg={COLORS.green}></text><text fg={COLORS.crust} bg={COLORS.green}> + </text><text fg={COLORS.green}></text>
        </Show>
      </box>
    </Show>
  </box>
}

function Pane(props: {
  readonly pane: PaneId
  readonly snapshot: SnapshotPayload
  readonly active: boolean
  readonly detailed: boolean
  readonly showOwners: boolean
  readonly iconMode: IconMode
  readonly tabs: readonly PaneTabPayload[]
  readonly tabsEnabled: boolean
  readonly onSortDirection: (pane: PaneId, key: PaneSortKey) => void
  readonly onSortCycle: (pane: PaneId, delta: -1 | 1) => void
  readonly onSelect: (pane: PaneId, index: number, toggle: boolean) => void
  readonly onActivateTab: (pane: PaneId, tabId: string) => void
  readonly onCloseTab: (pane: PaneId, tabId: string) => void
  readonly onNewTab: (pane: PaneId) => void
  readonly showQuickPreview: boolean
  readonly quickPreview?: PreviewPayload
  readonly onCopy: (text: string) => void
}) {
  const activeTabId = () => props.tabs.find((tab) => tab.active)?.id ?? `${props.pane}-default`
  return <box
    flexGrow={1}
    height="100%"
    flexDirection="column"
    border
    borderStyle="single"
    borderColor={props.active ? COLORS.lavender : COLORS.surface1}
    backgroundColor={COLORS.base}
  >
    <PaneTabs pane={props.pane} tabs={props.tabs} activePane={props.active} iconMode={props.iconMode} enabled={props.tabsEnabled} onActivate={props.onActivateTab} onClose={props.onCloseTab} onNew={props.onNewTab} />
    <PaneColumns pane={props.pane} snapshot={props.snapshot} detailed={props.detailed} showOwners={props.showOwners} iconMode={props.iconMode} onSortDirection={props.onSortDirection} onSortCycle={props.onSortCycle} />
    <Show when={props.showQuickPreview} fallback={<EntryList pane={props.pane} tabId={activeTabId()} snapshot={props.snapshot} detailed={props.detailed} showOwners={props.showOwners} iconMode={props.iconMode} onSelect={props.onSelect} />}>
      <QuickPreview preview={props.quickPreview} onCopy={props.onCopy} />
    </Show>
  </box>
}

function Workspace(props: {
  readonly workspace: WorkspaceSnapshotPayload
  readonly wide: boolean
  readonly detailed: boolean
  readonly showOwners: boolean
  readonly iconMode: IconMode
  readonly tabsEnabled: boolean
  readonly onSortDirection: (pane: PaneId, key: PaneSortKey) => void
  readonly onSortCycle: (pane: PaneId, delta: -1 | 1) => void
  readonly onSelect: (pane: PaneId, index: number, toggle: boolean) => void
  readonly onActivateTab: (pane: PaneId, tabId: string) => void
  readonly onCloseTab: (pane: PaneId, tabId: string) => void
  readonly onNewTab: (pane: PaneId) => void
  readonly quickPreviewPane?: PaneId
  readonly quickPreview?: PreviewPayload
  readonly onCopy: (text: string) => void
}) {
  const tabs = (pane: PaneId): readonly PaneTabPayload[] => props.workspace[`${pane}_tabs`] ?? [{
    id: "0",
    cwd_display: props.workspace[pane].cwd_display,
    active: true,
  }]
  const paneProps = (pane: PaneId) => ({
    pane,
    snapshot: props.workspace[pane],
    active: props.workspace.active_pane === pane,
    tabs: tabs(pane),
    tabsEnabled: props.tabsEnabled,
    iconMode: props.iconMode,
    showOwners: props.showOwners && hasOwnerGroup(props.workspace[pane]),
    onSortDirection: props.onSortDirection,
    onSortCycle: props.onSortCycle,
    onSelect: props.onSelect,
    onActivateTab: props.onActivateTab,
    onCloseTab: props.onCloseTab,
    onNewTab: props.onNewTab,
    showQuickPreview: props.quickPreviewPane !== undefined && props.quickPreviewPane !== pane,
    quickPreview: props.quickPreview,
    onCopy: props.onCopy,
  })
  return <box flexGrow={1} width="100%" flexDirection="row" gap={1}>
    <Show when={props.wide} fallback={props.workspace.active_pane === "left"
      ? <Pane {...paneProps("left")} detailed={false} />
      : <Pane {...paneProps("right")} detailed={false} />}>
      <Pane {...paneProps("left")} detailed={props.detailed} />
      <Pane {...paneProps("right")} detailed={props.detailed} />
    </Show>
  </box>
}

function ErrorPanel(props: { readonly message: string }) {
  return <box flexGrow={1} width="100%" flexDirection="column" justifyContent="center" alignItems="center" border borderStyle="single" borderColor={COLORS.red} backgroundColor={COLORS.base}>
    <text fg={COLORS.red}>CORE ERROR</text>
    <text fg={COLORS.text} wrapMode="word">{props.message}</text>
  </box>
}

function LoadingPanel() {
  return <box flexGrow={1} width="100%" flexDirection="column" justifyContent="center" alignItems="center" border borderStyle="single" borderColor={COLORS.surface1} backgroundColor={COLORS.base}>
    <text fg={COLORS.lavender}>CONNECTING TO CORE</text>
    <text fg={COLORS.subtext0}>Press F10, ZZ, or ZQ to cancel</text>
  </box>
}

function previewIsLoading(preview: PreviewPayload | undefined): boolean {
  return preview === undefined || preview.state === "queued" || preview.state === "running"
}

function Viewer(props: { readonly preview?: PreviewPayload; readonly quick?: boolean; readonly onCopy: (text: string) => void }) {
  const preview = () => props.preview
  const title = () => props.quick ? "SPACE QUICK VIEW" : "F3 VIEW"
  const markdown = () => preview()?.kind === "markdown" && preview()?.content !== undefined
  return <box flexGrow={1} width="100%" flexDirection="column" border borderStyle="double" borderColor={props.quick ? COLORS.teal : COLORS.lavender} backgroundColor={COLORS.base} paddingX={1} title={title()} titleColor={props.quick ? COLORS.teal : COLORS.lavender}>
    <text height={1} fg={COLORS.subtext0}>{preview() === undefined ? "loading preview" : `${preview()!.kind} · ${preview()!.state} · generation ${preview()!.generation}`}</text>
    <scrollbox flexGrow={1} width="100%" backgroundColor={COLORS.base} verticalScrollbarOptions={{ showArrows: false, trackOptions: { width: 1, foregroundColor: props.quick ? COLORS.teal : COLORS.lavender, backgroundColor: COLORS.surface0 } }}>
      <Show when={markdown()} fallback={<text id="preview-content" selectable selectionBg={COLORS.selected} selectionFg={COLORS.text} fg={COLORS.text} wrapMode="word">{previewIsLoading(preview()) ? "Loading preview..." : preview()!.content ?? preview()!.error_code ?? "Preview unavailable"}</text>}>
        <markdown id="preview-content" flexGrow={1} width="100%" fg={COLORS.text} content={preview()!.content!} syntaxStyle={SyntaxStyle.create()} conceal />
      </Show>
    </scrollbox>
    <PreviewFooter quick={props.quick === true} content={preview()?.content} onCopy={props.onCopy} />
  </box>
}

function ActionDialog(props: {
  readonly state: DialogState
  readonly onSubmit: (value?: string) => void
  readonly onCancel: () => void
}) {
  return <box flexGrow={1} width="100%" flexDirection="column" justifyContent="center" alignItems="center" backgroundColor={COLORS.mantle}>
    <box width="70%" flexDirection="column" border borderStyle="double" borderColor={props.state.kind === "delete" ? COLORS.red : COLORS.lavender} backgroundColor={COLORS.base} padding={1} title={props.state.kind === "mkdir" ? "F7 MKDIR" : props.state.kind === "mount-ssh" ? "F9 SSH" : props.state.kind === "search" ? (props.state.direction === 1 ? "/ SEARCH" : "? SEARCH") : props.state.kind === "rename" ? "cw RENAME" : "F8 DELETE"}>
      {props.state.kind === "search" ? <>
        <text fg={COLORS.text}>Search name</text>
        <input id="search-input" focused placeholder="file name" maxLength={255} onSubmit={(value) => props.onSubmit(typeof value === "string" ? value : undefined)} />
        <text fg={COLORS.subtext0}>Enter searches · Esc cancels · n/N repeat</text>
      </> : props.state.kind === "mkdir" ? <>
        <text fg={COLORS.text}>Directory name</text>
        <input id="mkdir-input" focused placeholder="new-directory" maxLength={255} onSubmit={(value) => props.onSubmit(typeof value === "string" ? value : undefined)} />
        <text fg={COLORS.subtext0}>Enter creates · Esc cancels</text>
      </> : props.state.kind === "mount-ssh" ? <>
        <text fg={COLORS.text}>Remote</text>
        <input id="mount-ssh-input" focused placeholder="user@host:/path" maxLength={16384} onSubmit={(value) => props.onSubmit(typeof value === "string" ? value : undefined)} />
        <text fg={COLORS.subtext0}>Enter mounts read-only · Esc cancels</text>
      </> : props.state.kind === "rename" ? <>
        <text fg={COLORS.text}>Rename {props.state.original}{props.state.total === undefined ? "" : ` (${props.state.step}/${props.state.total})`}</text>
        <input id="rename-input" focused value={props.state.rootOnly ? splitFileName(props.state.original).root : props.state.original} maxLength={255} onSubmit={(value) => props.onSubmit(typeof value === "string" ? value : undefined)} />
        <text fg={COLORS.subtext0}>{props.state.rootOnly ? `Enter renames root, keeps ${splitFileName(props.state.original).extension || "no extension"}` : "Enter renames"} · Esc cancels</text>
      </> : <>
        <text fg={COLORS.text}>Delete {props.state.target}?</text>
        <text fg={COLORS.subtext0}>Enter/Y confirms · Esc/N cancels</text>
      </>}
    </box>
  </box>
}

function ExitDialog(props: {
  readonly taskCount: number
  readonly stacked: boolean
  readonly onWait: () => void
  readonly onCancel: () => void
  readonly onReturn: () => void
}) {
  const button = (id: string, label: string, action: () => void, color: string) => <text
    id={id}
    fg={color}
    onMouseDown={(event) => {
      if (event.button !== MouseButton.LEFT) return
      event.preventDefault()
      event.stopPropagation()
      action()
    }}
  >[{label}]</text>
  return <box width="70%" flexDirection="column" border borderStyle="double" borderColor={COLORS.yellow} backgroundColor={COLORS.base} padding={1} title="EXIT">
    <text fg={COLORS.yellow}>TASKS STILL RUNNING ({props.taskCount})</text>
    <text fg={COLORS.text}>Choose how to close the session.</text>
    <box width="100%" flexDirection={props.stacked ? "column" : "row"} gap={2}>
      {button("exit-wait", "Wait for tasks", props.onWait, COLORS.green)}
      {button("exit-cancel", "Cancel and exit", props.onCancel, COLORS.red)}
      {button("exit-return", "Return", props.onReturn, COLORS.text)}
    </box>
  </box>
}

function actionTaskLabel(task: ActionTaskPayload): string {
  const source = taskPath(task.source_path_bytes_hex)
  const destination = taskPath(task.destination_path_bytes_hex)
  const current = taskPath(task.current_path_bytes_hex)
  const paths = source === undefined && current === undefined ? "" : ` ${source === undefined ? "" : shortTaskPath(source)}${destination === undefined ? "" : ` -> ${shortTaskPath(destination)}`}${current === undefined ? "" : ` [${shortTaskPath(current)}]`}`
  const bytes = task.bytes_known && task.bytes_completed !== undefined && task.bytes_total !== undefined
    ? ` ${formatFileSize(task.bytes_completed).trim()}/${formatFileSize(task.bytes_total).trim()}`
    : ""
  return `${task.action} #${task.task_id} ${task.state} ${task.completed_count}/${task.total_count}${bytes}${paths}${task.partial ? " partial" : ""}${task.error_code === undefined ? "" : ` ${task.error_code}`}`
}

function taskPath(pathBytesHex: string | undefined): string | undefined {
  if (pathBytesHex === undefined || pathBytesHex.length % 2 !== 0 || !/^(?:[0-9a-f]{2})*$/i.test(pathBytesHex)) return undefined
  const pairs = pathBytesHex.match(/../g)
  if (pairs === null) return undefined
  const bytes = Uint8Array.from(pairs, (pair) => Number.parseInt(pair, 16))
  const path = new TextDecoder("utf-8", { fatal: false }).decode(bytes)
  return path.replace(/[\u0000-\u001f\u007f]/g, "?")
}

function shortTaskPath(path: string): string {
  return path.length <= 36 ? path : `...${path.slice(-33)}`
}

function taskTimeLabel(task: ActionTaskPayload): string {
  if (task.started_at_unix_ms === undefined && task.finished_at_unix_ms === undefined) return ""
  const started = task.started_at_unix_ms === undefined ? "not started" : formatMtime(task.started_at_unix_ms)
  const finished = task.finished_at_unix_ms === undefined ? "running" : formatMtime(task.finished_at_unix_ms)
  return ` · Time ${started} -> ${finished}`
}

function resourceTaskLabel(task: ResourceTaskPayload): string {
  const target = task.mount_point ?? task.source_path ?? task.unmount_path ?? "resource"
  return `${task.resource} #${task.task_id} ${task.state} ${target}${task.error_code === undefined ? "" : ` ${task.error_code}`}`
}

function TaskCenter(props: {
  readonly actionTasks?: readonly ActionTaskPayload[]
  readonly resourceTasks?: readonly ResourceTaskPayload[]
  readonly onClose: () => void
  readonly onCommand: (command: CoreSessionCommand) => boolean | Promise<boolean> | void
}) {
  const [clearedHistory, setClearedHistory] = createSignal<ReadonlySet<string>>(new Set())
  const [view, setView] = createSignal<"queue" | "history">("queue")
  const [selectedTaskId, setSelectedTaskId] = createSignal<string | undefined>()
  const queue = () => (props.actionTasks ?? []).filter((task) => task.state === "queued" || task.state === "running")
  const history = () => (props.actionTasks ?? []).filter((task) =>
    task.state !== "queued" && task.state !== "running" && !clearedHistory().has(task.task_id))
  const selectedTask = () => {
    const taskId = selectedTaskId()
    return taskId === undefined ? undefined : history().find((task) => task.task_id === taskId)
  }
  const resourceQueue = () => (props.resourceTasks ?? []).filter((task) => task.state === "queued" || task.state === "running")
  const resourceHistory = () => (props.resourceTasks ?? []).filter((task) => task.state !== "queued" && task.state !== "running" && !clearedHistory().has(`resource-${task.task_id}`))
  const taskRows = (tasks: readonly ActionTaskPayload[], empty: string, cancelable: boolean) => <Show when={tasks.length !== 0} fallback={<text fg={COLORS.overlay1}>{empty}</text>}>
    <For each={tasks}>{(task) => <box
      id={`task-row-${task.task_id}`}
      width="100%"
      flexDirection="row"
      backgroundColor={selectedTaskId() === task.task_id ? COLORS.selected : COLORS.base}
      onMouseDown={(event) => {
        if (event.button !== MouseButton.LEFT) return
        event.preventDefault()
        event.stopPropagation()
        if (cancelable) props.onCommand({ action: "cancel-action", task_id: task.task_id })
        else setSelectedTaskId(task.task_id)
      }}
    >
      <text flexGrow={1} fg={task.state === "failed" || task.state === "cancelled" ? COLORS.red : COLORS.text}>{actionTaskLabel(task)}</text>
      <Show when={cancelable}><text fg={COLORS.yellow}> [x]</text></Show>
      </box>}</For>
  </Show>
  const details = (task: ActionTaskPayload) => <box
    id={`task-details-${task.task_id}`}
    width="100%"
    flexDirection="column"
    border
    borderStyle="single"
    borderColor={task.state === "failed" || task.state === "cancelled" ? COLORS.red : COLORS.surface1}
    paddingX={1}
  >
    <text fg={COLORS.yellow}>TASK DETAILS</text>
    <text fg={COLORS.text}>Task {task.task_id} · command {task.command_sequence}</text>
    <text fg={COLORS.text}>{task.action} · {task.state} · {task.pane}</text>
    <text fg={COLORS.subtext0}>Progress {task.completed_count}/{task.total_count}{task.bytes_known && task.bytes_completed !== undefined && task.bytes_total !== undefined ? ` · ${formatFileSize(task.bytes_completed).trim()}/${formatFileSize(task.bytes_total).trim()}` : ""}{task.partial ? " · partial" : ""}{taskTimeLabel(task)}</text>
    <Show when={task.source_path_bytes_hex !== undefined || task.destination_path_bytes_hex !== undefined || task.current_path_bytes_hex !== undefined}>
      <text fg={COLORS.text}>Transfer {taskPath(task.source_path_bytes_hex) ?? "[unreadable path]"}{task.destination_path_bytes_hex === undefined ? "" : ` -> ${taskPath(task.destination_path_bytes_hex) ?? "[unreadable path]"}`}{task.current_path_bytes_hex === undefined ? "" : ` · Current ${taskPath(task.current_path_bytes_hex) ?? "[unreadable path]"}`}</text>
    </Show>
    <Show when={task.undo_available === true}>
      <text fg={COLORS.green}>Undo available</text>
    </Show>
    <Show when={task.failed_index !== undefined || task.error_code !== undefined || task.os_error !== undefined}>
      <text fg={COLORS.red}>{task.failed_index === undefined ? "" : `Failed item ${task.failed_index + 1}`}{task.error_code === undefined ? "" : ` · Error ${task.error_code}`}{task.os_error === undefined ? "" : ` · OS error ${task.os_error}`}</text>
    </Show>
    <Show when={task.state === "failed" || task.state === "cancelled"}>
      <Show when={task.retryable} fallback={<text id={`task-retry-${task.task_id}`} fg={COLORS.overlay1}>Retry unavailable: core task identity is not retained</text>}>
        <text
          id={`task-retry-${task.task_id}`}
          fg={COLORS.yellow}
          onMouseDown={(event) => {
            if (event.button !== MouseButton.LEFT) return
            event.preventDefault()
            event.stopPropagation()
            props.onCommand({ action: "retry-action", task_id: task.task_id })
          }}
        >Retry task</text>
      </Show>
    </Show>
  </box>
  const resourceRows = (tasks: readonly ResourceTaskPayload[], cancelable: boolean) => <Show when={tasks.length !== 0} fallback={<text fg={COLORS.overlay1}>{cancelable ? "No queued or running resource tasks" : "No completed resource tasks"}</text>}>
    <For each={tasks}>{(task) => <box
      id={`resource-task-row-${task.task_id}`}
      width="100%"
      flexDirection="row"
      backgroundColor={COLORS.base}
      onMouseDown={(event) => {
        if (event.button !== MouseButton.LEFT) return
        event.preventDefault()
        event.stopPropagation()
        if (cancelable) props.onCommand({ action: "cancel-resource", task_id: task.task_id })
      }}
    >
      <text flexGrow={1} fg={task.state === "failed" || task.state === "cancelled" ? COLORS.red : COLORS.text}>{resourceTaskLabel(task)}</text>
      <Show when={cancelable}><text fg={COLORS.yellow}> [x]</text></Show>
    </box>}</For>
  </Show>
  const tab = (kind: "queue" | "history", label: string, count: () => number) => <text
    id={`task-${kind}-tab`}
    bg={view() === kind ? COLORS.yellow : COLORS.surface1}
    fg={view() === kind ? COLORS.crust : COLORS.text}
    onMouseDown={(event) => {
      if (event.button !== MouseButton.LEFT) return
      event.preventDefault()
      event.stopPropagation()
      setView(kind)
    }}
  > {label} ({count()}) </text>
  return <box width="100%" height="100%" flexDirection="column" border borderStyle="double" borderColor={COLORS.lavender} backgroundColor={COLORS.base} padding={1} title="TASK CENTER" titleColor={COLORS.lavender} onKeyDown={(key) => {
    if (key.name.toLowerCase() === "escape" || key.sequence === "\u001b") {
      key.preventDefault()
      key.stopPropagation()
      props.onClose()
    }
  }}>
    <box width="100%" height={1} flexDirection="row" gap={1}>
      {tab("queue", "QUEUE", () => queue().length + resourceQueue().length)}
      {tab("history", "HISTORY", () => history().length + resourceHistory().length)}
    </box>
    <scrollbox flexGrow={1} width="100%" verticalScrollbarOptions={{ showArrows: false, trackOptions: { width: 1, foregroundColor: COLORS.yellow, backgroundColor: COLORS.surface0 } }}>
      <Show when={view() === "queue"} fallback={<>
        <box height={1} width="100%" flexDirection="row">
          <text flexGrow={1} fg={COLORS.yellow}>Completed actions</text>
          <text id="task-clear-history" fg={COLORS.yellow} onMouseDown={(event) => {
            if (event.button !== MouseButton.LEFT) return
            event.preventDefault()
            event.stopPropagation()
            setClearedHistory(new Set([
              ...(props.actionTasks ?? [])
              .filter((task) => task.state !== "queued" && task.state !== "running")
              .map((task) => task.task_id),
              ...(props.resourceTasks ?? [])
                .filter((task) => task.state !== "queued" && task.state !== "running")
                .map((task) => `resource-${task.task_id}`),
            ]))
            setSelectedTaskId(undefined)
          }}>Clear</text>
        </box>
        {taskRows(history(), "No completed actions", false)}
        {resourceRows(resourceHistory(), false)}
        <Show when={selectedTask() !== undefined}>
          {details(selectedTask()!)}
        </Show>
      </>}>
        {taskRows(queue(), "No queued or running file actions", true)}
        {resourceRows(resourceQueue(), true)}
      </Show>
    </scrollbox>
    <text height={1} fg={COLORS.subtext0}>Choose Queue/History; Esc closes</text>
  </box>
}

function StatusBar(props: {
  readonly workspace?: WorkspaceSnapshotPayload
  readonly tasks?: readonly PreviewTaskPayload[]
  readonly actionTasks?: readonly ActionTaskPayload[]
  readonly resourceTasks?: readonly ResourceTaskPayload[]
  readonly compact: boolean
  readonly notice?: string
  readonly visual: boolean
  readonly iconMode: IconMode
  readonly pathMode: StatusPathMode
  readonly homeDirectory?: string
  readonly onTogglePath: () => void
  readonly onCopyPath: (path: string) => void
  readonly onOpenTasks: () => void
}) {
  const active = () => props.workspace?.active_pane ?? "left"
  const snapshot = () => active() === "left" ? props.workspace?.left : props.workspace?.right
  const displayedPath = () => formatStatusPath(snapshot()?.cwd_display ?? "connecting", props.homeDirectory, props.pathMode)
  const pathMouseDown = (event: { button: number; preventDefault(): void; stopPropagation(): void }) => {
    if (event.button !== MouseButton.LEFT && event.button !== MouseButton.RIGHT) return
    event.preventDefault()
    event.stopPropagation()
    if (event.button === MouseButton.RIGHT) props.onCopyPath(displayedPath())
    else props.onTogglePath()
  }
  const latestTask = () => props.tasks?.at(-1)
  const latestAction = () => props.actionTasks?.at(-1)
  const latestResource = () => props.resourceTasks?.at(-1)
  const queuedActions = () => props.actionTasks?.filter((task) => task.state === "queued" || task.state === "running").length ?? 0
  const queuedResources = () => props.resourceTasks?.filter((task) => task.state === "queued" || task.state === "running").length ?? 0
  const taskCount = () => (props.actionTasks?.length ?? 0) + (props.resourceTasks?.length ?? 0)
  const taskMouseDown = (event: { button: number; preventDefault(): void; stopPropagation(): void }) => {
    if (event.button !== MouseButton.LEFT) return
    event.preventDefault()
    event.stopPropagation()
    props.onOpenTasks()
  }
  const detail = () => props.notice ?? (latestAction() !== undefined
    ? `${latestAction()!.action} ${latestAction()!.state} ${latestAction()!.completed_count}/${latestAction()!.total_count}${latestAction()!.partial ? " partial" : ""}${latestAction()!.error_code === undefined ? "" : ` ${latestAction()!.error_code}`}`
    : latestResource() !== undefined ? `${latestResource()!.resource} ${latestResource()!.state}`
    : latestTask() === undefined ? `${snapshot()?.selection_count ?? 0} selected` : `task ${latestTask()!.state}`)
  const showDetail = () => props.notice !== undefined || !props.compact
    || latestAction()?.state === "failed" || latestAction()?.state === "cancelled" || latestAction()?.partial === true
  const modeLabel = () => props.visual ? " VISUAL " : " NORMAL "
  if (props.iconMode === "ascii") {
    return <box width="100%" height={1} flexDirection="row" backgroundColor={COLORS.surface0}>
      <text bg={COLORS.mauve} fg={COLORS.crust}>{modeLabel()}</text>
      <text id="status-path" flexGrow={1} bg={COLORS.surface0} fg={COLORS.text} truncate onMouseDown={pathMouseDown}> {displayedPath()} </text>
      <text bg={COLORS.green} fg={COLORS.crust}> {snapshot()?.entry_count ?? 0} items </text>
      <text id="tasks-entry" bg={queuedActions() + queuedResources() !== 0 ? COLORS.yellow : COLORS.surface1} fg={COLORS.crust} onMouseDown={taskMouseDown}> Tasks {queuedActions() + queuedResources()}/{taskCount()} </text>
      <Show when={showDetail()}>
        <text bg={COLORS.surface1} fg={COLORS.text}> {detail()} </text>
      </Show>
    </box>
  }
  return <box width="100%" height={1} flexDirection="row" backgroundColor={COLORS.crust}>
    <text fg={COLORS.mauve} bg={COLORS.crust}></text>
    <text bg={COLORS.mauve} fg={COLORS.crust}>{modeLabel()}</text>
    <text fg={COLORS.mauve} bg={COLORS.surface0}></text>
    <text id="status-path" flexGrow={1} bg={COLORS.surface0} fg={COLORS.text} truncate onMouseDown={pathMouseDown}> {displayedPath()} </text>
    <text fg={COLORS.surface0} bg={COLORS.green}></text>
    <text bg={COLORS.green} fg={COLORS.crust}> {snapshot()?.entry_count ?? 0} items </text>
    <text id="tasks-entry" fg={COLORS.crust} bg={queuedActions() !== 0 ? COLORS.yellow : COLORS.surface1} onMouseDown={taskMouseDown}> Tasks {queuedActions()}/{taskCount()} </text>
    <Show when={showDetail()}>
      <text fg={COLORS.green} bg={COLORS.surface1}></text>
      <text bg={COLORS.surface1} fg={COLORS.text}> {detail()} </text>
      <text fg={COLORS.surface1} bg={COLORS.crust}></text>
    </Show>
    <Show when={props.compact && !showDetail()}><text fg={COLORS.green} bg={COLORS.crust}></text></Show>
  </box>
}

function FunctionKey(props: {
  readonly action: FunctionAction
  readonly keyName: string
  readonly label: string
  readonly enabled?: boolean
  readonly compact?: boolean
  readonly iconMode: IconMode
  readonly onAction: (action: FunctionAction) => void
}) {
  const enabled = () => props.enabled !== false
  const narrowLabel = () => props.compact && (props.keyName === "F3" || props.keyName === "F10")
    ? `${props.keyName} ${props.label}`
    : props.keyName
  return <box
    id={`function-${props.action}`}
    flexGrow={1}
    height={1}
    flexDirection="row"
    onMouseDown={(event) => {
      event.preventDefault()
      event.stopPropagation()
      if (event.button === MouseButton.LEFT && enabled()) props.onAction(props.action)
    }}
  >
    <Show when={props.iconMode !== "ascii"} fallback={<text fg={enabled() ? COLORS.sapphire : COLORS.overlay1}>{props.compact ? `[${narrowLabel()}]` : `[${props.keyName} ${props.label}]`}</text>}>
      <text fg={enabled() ? COLORS.sapphire : COLORS.surface1} bg={COLORS.crust}></text>
      <text bg={enabled() ? COLORS.sapphire : COLORS.surface1} fg={enabled() ? COLORS.crust : COLORS.overlay1}>{props.compact ? narrowLabel() : `${props.keyName} ${props.label}`}</text>
      <text fg={enabled() ? COLORS.sapphire : COLORS.surface1} bg={COLORS.crust}></text>
    </Show>
  </box>
}

const FUNCTION_KEYS: readonly Readonly<{ action: FunctionAction; keyName: string; label: string }>[] = [
  { action: "view", keyName: "F3", label: "View" },
  { action: "edit", keyName: "F4", label: "Edit" },
  { action: "copy", keyName: "F5", label: "Copy" },
  { action: "move", keyName: "F6", label: "Move" },
  { action: "mkdir", keyName: "F7", label: "MkDir" },
  { action: "delete", keyName: "F8", label: "Delete" },
  { action: "mount-ssh", keyName: "F9", label: "SSH" },
  { action: "quit", keyName: "F10", label: "Quit" },
]

function FunctionRow(props: {
  readonly keys: typeof FUNCTION_KEYS
  readonly canView: boolean
  readonly canFileActions: boolean
  readonly canResourceTasks: boolean
  readonly iconMode: IconMode
  readonly compact?: boolean
  readonly onAction: (action: FunctionAction) => void
}) {
  const enabled = (action: FunctionAction) => action === "view"
    ? props.canView
    : action === "copy" || action === "move" || action === "mkdir" || action === "delete"
      ? props.canFileActions
      : action === "mount-ssh"
        ? props.canResourceTasks
      : true
  return <box width="100%" height={1} flexDirection="row">
    <For each={props.keys}>{(item) => <FunctionKey {...item} compact={props.compact} enabled={enabled(item.action)} iconMode={props.iconMode} onAction={props.onAction} />}</For>
  </box>
}

function BottomBars(props: {
  readonly workspace?: WorkspaceSnapshotPayload
  readonly tasks?: readonly PreviewTaskPayload[]
  readonly actionTasks?: readonly ActionTaskPayload[]
  readonly resourceTasks?: readonly ResourceTaskPayload[]
  readonly compact: boolean
  readonly stacked: boolean
  readonly notice?: string
  readonly visual: boolean
  readonly canView: boolean
  readonly canFileActions: boolean
  readonly canResourceTasks: boolean
  readonly iconMode: IconMode
  readonly onAction: (action: FunctionAction) => void
  readonly pathMode: StatusPathMode
  readonly homeDirectory?: string
  readonly onTogglePath: () => void
  readonly onCopyPath: (path: string) => void
  readonly onOpenTasks: () => void
}) {
  return <box width="100%" height={3} flexDirection="column">
    <StatusBar workspace={props.workspace} tasks={props.tasks} actionTasks={props.actionTasks} resourceTasks={props.resourceTasks} compact={props.compact} notice={props.notice} visual={props.visual} iconMode={props.iconMode} pathMode={props.pathMode} homeDirectory={props.homeDirectory} onTogglePath={props.onTogglePath} onCopyPath={props.onCopyPath} onOpenTasks={props.onOpenTasks} />
    <box id="bottom-divider" width="100%" height={1} border={["top"]} borderStyle="single" borderColor={COLORS.divider} />
    <FunctionRow keys={FUNCTION_KEYS} compact={props.stacked} canView={props.canView} canFileActions={props.canFileActions} canResourceTasks={props.canResourceTasks} iconMode={props.iconMode} onAction={props.onAction} />
  </box>
}

export function App(props: AppProps) {
  const renderer = useRenderer()
  const dimensions = useTerminalDimensions()
  const wide = () => dimensions().width >= 80
  const detailed = () => dimensions().width >= 160
  const iconMode = (): IconMode => props.iconMode ?? (process.env.NEOVIFM_ICONS === "ascii" ? "ascii" : "fancy")
  const canSort = () => props.capabilities?.includes("workspace-sort-v1") === true
  const canTabs = () => props.capabilities?.includes("pane-tabs-v1") === true
  const canFileActions = () => props.capabilities?.includes("file-actions-v1") === true
  const canRename = () => props.capabilities?.includes("file-rename-v1") === true
  const canResourceTasks = () => props.capabilities?.includes("resource-tasks-v1") === true
  // Keep multi-key Vifm prefixes alive while task/preview records cause rerenders.
  const keymap = createMemo(() => new VifmKeymap())
  const [viewerOpen, setViewerOpen] = createSignal(false)
  const [quickPreviewOpen, setQuickPreviewOpen] = createSignal(false)
  const [taskCenterOpen, setTaskCenterOpen] = createSignal(false)
  const [exitPrompt, setExitPrompt] = createSignal(false)
  const [exitAfterTasks, setExitAfterTasks] = createSignal(false)
  const [notice, setNotice] = createSignal<string | undefined>()
  const [dialog, setDialog] = createSignal<DialogState | undefined>()
  const [visual, setVisual] = createSignal(false)
  let visualAnchor = -1
  const [pathMode, setPathMode] = createSignal<StatusPathMode>("absolute")
  let quickPreviewIdentity = ""
  let handledOpenSequence = 0
  let yankBuffer: YankBuffer | undefined

  const activeSnapshot = () => props.workspace?.active_pane === "right" ? props.workspace.right : props.workspace?.left
  const currentEntry = () => {
    const snapshot = activeSnapshot()
    return snapshot === undefined || snapshot.cursor < 0 ? undefined : snapshot.entries[snapshot.cursor]
  }
  const matchingPreview = () => {
    const preview = props.preview
    const workspace = props.workspace
    const entry = currentEntry()
    if (preview === undefined || workspace === undefined || entry === undefined) return undefined
    const targetPane = quickPreviewOpen()
      ? workspace.active_pane === "left" ? "right" : "left"
      : workspace.active_pane
    return preview.pane === workspace.active_pane && (preview.target_pane ?? preview.pane) === targetPane && preview.path_bytes_hex === entry.path_bytes_hex
      ? preview
      : undefined
  }
  const actionTargets = (snapshot: SnapshotPayload): readonly CoreActionTarget[] | undefined => {
    const entries = snapshot.selection_count !== 0
      ? snapshot.entries.filter((entry) => entry.selected)
      : snapshot.cursor < 0 ? [] : [snapshot.entries[snapshot.cursor]!]
    if (entries.some((entry) => entry.device === undefined || entry.inode === undefined || entry.ctime_unix_ns === undefined)) return undefined
    return entries.map((entry) => ({
      path_bytes_hex: entry.path_bytes_hex,
      device: entry.device!,
      inode: entry.inode!,
      ctime_unix_ns: entry.ctime_unix_ns!,
      kind: entry.kind,
    }))
  }
  const actionContext = (snapshot: SnapshotPayload) => snapshot.cwd_device === undefined || snapshot.cwd_inode === undefined || snapshot.cwd_ctime_unix_ns === undefined || snapshot.snapshot_revision === "0"
    ? undefined
    : {
        cwd_bytes_hex: snapshot.cwd_bytes_hex,
        snapshot_revision: snapshot.snapshot_revision,
        cwd_device: snapshot.cwd_device,
        cwd_inode: snapshot.cwd_inode,
        cwd_ctime_unix_ns: snapshot.cwd_ctime_unix_ns,
      }
  const pathFromIdentity = (pathBytesHex: string): string | undefined => {
    if (pathBytesHex.length === 0 || pathBytesHex.length % 2 !== 0 || !/^[0-9a-f]+$/i.test(pathBytesHex)) return undefined
    const pairs = pathBytesHex.match(/../g)
    if (pairs === null) return undefined
    const bytes = Uint8Array.from(pairs, (pair) => Number.parseInt(pair, 16))
    try {
      const path = new TextDecoder("utf-8", { fatal: true }).decode(bytes)
      return path.includes("\0") ? undefined : path
    } catch {
      return undefined
    }
  }
  const sendCommand = (command: CoreSessionCommand, successNotice?: string) => {
    if ((command.action === "sort-by" || command.action === "sort-cycle") && !canSort()) {
      setNotice("Core sorting is unavailable")
      return false
    }
    if ((command.action === "tab-cycle" || command.action === "new-tab" || command.action === "activate-tab" || command.action === "close-tab") && !canTabs()) {
      setNotice("Core pane tabs are unavailable")
      return false
    }
    if (command.action === "select-entry" && !canTabs()) {
      setNotice("Core mouse selection is unavailable")
      return false
    }
    const sent = props.onCommand?.(command)
    if (sent instanceof Promise) {
      void sent.then((accepted) => {
        if (!accepted) setNotice("Core command channel is unavailable")
        else if (successNotice !== undefined) setNotice(successNotice)
      })
      return true
    }
    if (sent === false) {
      setNotice("Core command channel is unavailable")
      return false
    }
    if (successNotice !== undefined) setNotice(successNotice)
    return true
  }
  const activeTaskCount = () => (props.actionTasks?.filter((task) => task.state === "queued" || task.state === "running").length ?? 0) +
    (props.resourceTasks?.filter((task) => task.state === "queued" || task.state === "running").length ?? 0)
  const quitNow = () => {
    setExitAfterTasks(false)
    setExitPrompt(false)
    props.onCancel?.()
    renderer.destroy()
  }
  const quit = () => {
    if (activeTaskCount() !== 0) {
      setExitPrompt(true)
      return
    }
    quitNow()
  }
  const waitForTasks = () => {
    setExitPrompt(false)
    setExitAfterTasks(true)
    setNotice("Waiting for tasks to finish")
  }
  const cancelAndQuit = () => {
    setExitPrompt(false)
    setExitAfterTasks(true)
    for (const task of props.actionTasks ?? []) {
      if (task.state === "queued" || task.state === "running") sendCommand({ action: "cancel-action", task_id: task.task_id })
    }
    for (const task of props.resourceTasks ?? []) {
      if (task.state === "queued" || task.state === "running") sendCommand({ action: "cancel-resource", task_id: task.task_id })
    }
    setNotice("Cancelling tasks before exit")
  }
  createEffect(() => {
    if (exitAfterTasks() && activeTaskCount() === 0) quitNow()
  })
  const requestQuickPreview = () => {
    if (!quickPreviewOpen()) {
      quickPreviewIdentity = ""
      return
    }
    const workspace = props.workspace
    const entry = currentEntry()
    if (workspace === undefined || entry === undefined) return
    const source = workspace.active_pane === "left" ? workspace.left : workspace.right
    const identity = actionContext(source)
    const targetPane = workspace.active_pane === "left" ? "right" : "left"
    if (identity === undefined || entry.device === undefined || entry.inode === undefined || entry.ctime_unix_ns === undefined) {
      quickPreviewIdentity = ""
      return
    }
    const key = `${workspace.active_pane}:${targetPane}:${source.snapshot_revision}:${entry.path_bytes_hex}`
    if (key === quickPreviewIdentity) return
    quickPreviewIdentity = key
    sendCommand({
      action: "preview",
      pane: workspace.active_pane,
      target_pane: targetPane,
      ...identity,
      path_bytes_hex: entry.path_bytes_hex,
      device: entry.device,
      inode: entry.inode,
      ctime_unix_ns: entry.ctime_unix_ns,
    })
  }
  createEffect(requestQuickPreview)
  const copyStatusPath = (path: string) => {
    if (props.onCopyText === undefined) {
      setNotice("Clipboard is unavailable")
      return
    }
    void Promise.resolve().then(() => props.onCopyText!(path)).then(
      () => setNotice(`Copied ${path}`),
      (error) => setNotice(`Copy failed: ${error instanceof Error ? error.message : String(error)}`),
    )
  }
  const copyPreview = (content: string) => {
    if (props.onCopyText === undefined) {
      setNotice("Clipboard is unavailable")
      return
    }
    void Promise.resolve().then(() => props.onCopyText!(content)).then(
      () => setNotice("Preview copied"),
      (error) => setNotice(`Copy failed: ${error instanceof Error ? error.message : String(error)}`),
    )
  }
  const copySelectedPreview = () => {
    const content = renderer.getSelection()?.getSelectedText() ?? ""
    if (content.length === 0) setNotice("No preview text selected")
    else copyPreview(content)
  }
  const dispatchFunction = (action: FunctionAction) => {
    if (dialog() !== undefined) return
    if (action === "quit") {
      quit()
      return
    }
    if (action === "quick-view") {
      setViewerOpen(false)
      setQuickPreviewOpen((open) => !open)
      setNotice(undefined)
      return
    }
    if (action === "view") {
      setQuickPreviewOpen(false)
      setViewerOpen((open) => !open)
      setNotice(undefined)
      return
    }
    if (action === "edit") {
      const entry = currentEntry()
      if (entry === undefined || entry.kind === "directory") {
        setNotice("Edit requires a file")
      } else if (props.onEdit === undefined) {
        setNotice("Editor unavailable")
      } else {
        const path = pathFromIdentity(entry.path_bytes_hex)
        if (path === undefined) {
          setNotice("Editor unavailable for a non-UTF-8 path")
          return
        }
        renderer.suspend()
        void Promise.resolve().then(() => props.onEdit!(path)).then(
          () => setNotice(`Edited ${entry.name_display}`),
          (error) => setNotice(`Edit failed: ${error instanceof Error ? error.message : String(error)}`),
        ).finally(() => renderer.resume())
      }
      return
    }
    if (action === "mount-ssh") {
      if (!canResourceTasks()) {
        setNotice("SSH mount is unavailable")
        return
      }
      const workspace = props.workspace
      if (workspace === undefined) {
        setNotice("SSH mount requires a workspace")
        return
      }
      setDialog({ kind: "mount-ssh", pane: workspace.active_pane })
      return
    }
    if (action === "yank") {
      const snapshot = activeSnapshot()
      const buffer = snapshot === undefined ? undefined : yankFromSnapshot(snapshot)
      if (buffer === undefined) {
        setNotice("Nothing to yank")
        return
      }
      yankBuffer = buffer
      setNotice(`${buffer.entries.length} item(s) yanked`)
      return
    }
    if (!canFileActions()) {
      setNotice("Core file actions are unavailable")
      return
    }
    if (action === "mkdir") {
      const workspace = props.workspace
      if (workspace === undefined) {
        setNotice("MkDir requires a workspace")
        return
      }
      const pane = workspace.active_pane
      const snapshot = pane === "left" ? workspace.left : workspace.right
      const context = actionContext(snapshot)
      if (context === undefined) {
        setNotice("MkDir requires a stable core snapshot")
        return
      }
      setDialog({ kind: "mkdir", pane, ...context })
      return
    }
    if (action === "delete") {
      const entry = currentEntry()
      if (entry === undefined) setNotice("Delete requires a file")
      else {
        const workspace = props.workspace!
        const pane = workspace.active_pane
        const snapshot = pane === "left" ? workspace.left : workspace.right
        const context = actionContext(snapshot)
        const targets = actionTargets(snapshot)
        if (context === undefined || targets === undefined) {
          setNotice("Delete requires a stable core snapshot")
          return
        }
        const selectionCount = snapshot.selection_count
        setDialog({
          kind: "delete",
          target: selectionCount === 0 ? entry.name_display : `${selectionCount} selected items`,
          command: {
            action: "delete",
            pane,
            ...context,
            targets,
          },
        })
      }
      return
    }
    if (action === "put-copy" || action === "put-move") {
      const move = action === "put-move"
      const label = move ? "Put (move)" : "Put"
      if (yankBuffer === undefined) {
        setNotice("Yank buffer is empty")
        return
      }
      const workspace = props.workspace
      if (workspace === undefined) {
        setNotice(`${label} requires a workspace`)
        return
      }
      const destination = activeSnapshot()!
      const destinationContext = actionContext(destination)
      if (destinationContext === undefined) {
        setNotice(`${label} requires a stable core snapshot`)
        return
      }
      const resolved = resolvePutSource(yankBuffer, workspace)
      if (!resolved.ok) {
        if (resolved.reason === "source-not-visible") {
          setNotice("Yank source directory is no longer visible")
        } else if (resolved.reason === "stale-targets") {
          const extra = resolved.missing.length - 1
          setNotice(`Yank source no longer exists: ${resolved.missing[0]}${extra > 0 ? ` (+${extra} more)` : ""}`)
        } else {
          setNotice(`${label} requires a stable core snapshot`)
        }
        return
      }
      sendCommand({
        action: move ? "move-files" : "copy",
        pane: resolved.source.pane,
        cwd_bytes_hex: resolved.source.cwd_bytes_hex,
        snapshot_revision: resolved.source.snapshot_revision,
        cwd_device: resolved.source.cwd_device,
        cwd_inode: resolved.source.cwd_inode,
        cwd_ctime_unix_ns: resolved.source.cwd_ctime_unix_ns,
        destination_cwd_bytes_hex: destination.cwd_bytes_hex,
        destination_snapshot_revision: destinationContext.snapshot_revision,
        destination_cwd_device: destinationContext.cwd_device,
        destination_cwd_inode: destinationContext.cwd_inode,
        destination_cwd_ctime_unix_ns: destinationContext.cwd_ctime_unix_ns,
        targets: resolved.targets,
      }, `${label} requested`)
      return
    }
    if (action === "rename" || action === "rename-root") {
      if (!canRename()) {
        setNotice("Rename is unavailable")
        return
      }
      const workspace = props.workspace
      const entry = currentEntry()
      if (workspace === undefined || entry === undefined) {
        setNotice("Rename requires a file")
        return
      }
      const snapshot = activeSnapshot()!
      const selected = snapshot.entries.filter((candidate) => candidate.selected)
      if (selected.length !== 0) {
        // Batch rename stays protocol-single-target: one dialog per entry, in
        // selection order. Identity is re-resolved per submit, so the queue
        // only needs the path hex of each pending item.
        const first = selected[0]!
        setDialog({
          kind: "rename",
          rootOnly: action === "rename-root",
          pane: workspace.active_pane,
          original: first.name_display,
          path_bytes_hex: first.path_bytes_hex,
          queue: selected.slice(1).map((candidate) => candidate.path_bytes_hex),
          step: 1,
          total: selected.length,
        })
        return
      }
      const context = actionContext(snapshot)
      if (context === undefined || entry.device === undefined || entry.inode === undefined || entry.ctime_unix_ns === undefined) {
        setNotice("Rename requires a stable core snapshot")
        return
      }
      // Identity is re-resolved on submit so watcher refreshes while typing do
      // not stale the command.
      setDialog({
        kind: "rename",
        rootOnly: action === "rename-root",
        pane: workspace.active_pane,
        original: entry.name_display,
        path_bytes_hex: entry.path_bytes_hex,
      })
      return
    }
    const workspace = props.workspace
    if (workspace === undefined) {
      setNotice(`${action === "move" ? "Move" : "Copy"} requires a workspace`)
      return
    }
    const pane = workspace.active_pane
    const snapshot = pane === "left" ? workspace.left : workspace.right
    const destination = pane === "left" ? workspace.right : workspace.left
    const context = actionContext(snapshot)
    const destinationContext = actionContext(destination)
    const targets = actionTargets(snapshot)
    if (targets?.length === 0) {
      setNotice(`${action === "move" ? "Move" : "Copy"} requires a file`)
      return
    }
    if (context === undefined || destinationContext === undefined || targets === undefined) {
      setNotice(`${action === "move" ? "Move" : "Copy"} requires stable core snapshots`)
      return
    }
    sendCommand({
      action: action === "move" ? "move-files" : "copy",
      pane,
      ...context,
      destination_cwd_bytes_hex: destination.cwd_bytes_hex,
      destination_snapshot_revision: destinationContext.snapshot_revision,
      destination_cwd_device: destinationContext.cwd_device,
      destination_cwd_inode: destinationContext.cwd_inode,
      destination_cwd_ctime_unix_ns: destinationContext.cwd_ctime_unix_ns,
      targets,
    }, `${action === "move" ? "Move" : "Copy"} requested`)
  }
  const openCurrentFile = () => {
    const workspace = props.workspace
    const entry = currentEntry()
    if (workspace === undefined || entry === undefined || entry.kind === "directory") {
      setNotice("Open requires a file")
      return
    }
    if (props.capabilities?.includes("open-v1") === true && props.onCommand !== undefined) {
      const context = actionContext(activeSnapshot()!)
      if (context === undefined || entry.device === undefined || entry.inode === undefined || entry.ctime_unix_ns === undefined) {
        setNotice("Open requires a stable core snapshot")
        return
      }
      sendCommand({
        action: "open",
        intent: "open",
        pane: workspace.active_pane,
        ...context,
        path_bytes_hex: entry.path_bytes_hex,
        device: entry.device,
        inode: entry.inode,
        ctime_unix_ns: entry.ctime_unix_ns,
      }, `Open ${entry.name_display} requested`)
      return
    }
    if (props.onOpen === undefined) {
      setNotice("System opener unavailable")
      return
    }
    const path = pathFromIdentity(entry.path_bytes_hex)
    if (path === undefined) {
      setNotice("Open unavailable for a non-UTF-8 path")
      return
    }
    void Promise.resolve().then(() => props.onOpen!(path)).then(
      () => setNotice(`Opened ${entry.name_display}`),
      (error) => setNotice(`Open failed: ${error instanceof Error ? error.message : String(error)}`),
    )
  }
  createEffect(() => {
    const resolved = props.open
    if (resolved === undefined || resolved.command_sequence <= handledOpenSequence) return
    handledOpenSequence = resolved.command_sequence
    const launch = props.onOpenResolved !== undefined
      ? () => props.onOpenResolved!(resolved.argv)
      : () => {
          const path = pathFromIdentity(resolved.path_bytes_hex)
          if (path === undefined || props.onOpen === undefined) throw new Error("System opener unavailable")
          return props.onOpen(path)
        }
    void Promise.resolve().then(launch).then(
      () => setNotice("Opened"),
      (error) => setNotice(`Open failed: ${error instanceof Error ? error.message : String(error)}`),
    )
  })
  // Advances a batch rename to the next queued path that still exists in the
  // latest workspace; vanished entries are skipped and an exhausted queue
  // closes the dialog.
  const nextRenameState = (state: Extract<DialogState, { kind: "rename" }>): DialogState | undefined => {
    const queue = state.queue ?? []
    const workspace = props.workspace
    const snapshot = workspace === undefined ? undefined : state.pane === "left" ? workspace.left : workspace.right
    for (let skipped = 0; skipped < queue.length; skipped++) {
      const pathBytesHex = queue[skipped]!
      const entry = snapshot?.entries.find((candidate) => candidate.path_bytes_hex === pathBytesHex)
      if (entry === undefined) continue
      return {
        kind: "rename",
        rootOnly: state.rootOnly,
        pane: state.pane,
        original: entry.name_display,
        path_bytes_hex: entry.path_bytes_hex,
        queue: queue.slice(skipped + 1),
        step: (state.step ?? 1) + skipped + 1,
        total: state.total,
      }
    }
    return undefined
  }
  const closeDialog = () => setDialog(undefined)
  const submitDialog = (value?: string) => {
    const state = dialog()
    if (state?.kind === "search") {
      const query = value ?? ""
      if (query.length === 0 || query.length > 255 || query.includes("\0") || query.includes("\n") || query.includes("\r")) {
        setNotice("Invalid search query")
        return
      }
      sendCommand({ action: "search", query, direction: state.direction })
    } else if (state?.kind === "mkdir") {
      const name = value?.trim() ?? ""
      if (name.length === 0 || new TextEncoder().encode(name).byteLength > 255 ||
        name === "." || name === ".." || name.includes("/") || name.includes("\\") || name.includes("\0")) {
        setNotice("Invalid directory name")
        return
      }
      sendCommand({
        action: "mkdir",
        pane: state.pane,
        cwd_bytes_hex: state.cwd_bytes_hex,
        snapshot_revision: state.snapshot_revision,
        cwd_device: state.cwd_device,
        cwd_inode: state.cwd_inode,
        cwd_ctime_unix_ns: state.cwd_ctime_unix_ns,
        name,
      }, `Create ${name} requested`)
    } else if (state?.kind === "mount-ssh") {
      const remote = value?.trim() ?? ""
      if (remote.length === 0 || remote.length > 16384 || remote.includes("\0") ||
        remote.includes("\n") || remote.includes("\r") || remote.startsWith("-")) {
        setNotice("Invalid remote")
        return
      }
      sendCommand({ action: "mount-ssh", pane: state.pane, remote }, "SSH mount requested")
    } else if (state?.kind === "rename") {
      const typed = value?.trim() ?? ""
      const name = state.rootOnly ? `${typed}${splitFileName(state.original).extension}` : typed
      if (name.length === 0 || new TextEncoder().encode(name).byteLength > 255 ||
        name === "." || name === ".." || name.includes("/") || name.includes("\\") || name.includes("\0")) {
        setNotice("Invalid name")
        return
      }
      if (name === state.original) {
        setDialog(nextRenameState(state))
        return
      }
      // Re-resolve the entry and pane identity against the latest workspace:
      // the user may have typed while a watcher refresh landed.
      const workspace = props.workspace
      const snapshot = workspace === undefined ? undefined : state.pane === "left" ? workspace.left : workspace.right
      const entry = snapshot?.entries.find((candidate) => candidate.path_bytes_hex === state.path_bytes_hex)
      const context = snapshot === undefined ? undefined : actionContext(snapshot)
      if (workspace === undefined || snapshot === undefined || entry === undefined ||
        context === undefined || entry.device === undefined || entry.inode === undefined || entry.ctime_unix_ns === undefined) {
        setNotice("Rename source no longer exists")
        setDialog(nextRenameState(state))
        return
      }
      sendCommand({
        action: "rename",
        pane: state.pane,
        ...context,
        targets: [{
          path_bytes_hex: entry.path_bytes_hex,
          device: entry.device,
          inode: entry.inode,
          ctime_unix_ns: entry.ctime_unix_ns,
          kind: entry.kind,
        }],
        name,
      }, `Rename requested`)
      setDialog(nextRenameState(state))
      return
    } else if (state?.kind === "delete") {
      sendCommand(state.command, `Delete ${state.target} requested`)
    }
    closeDialog()
  }

  useKeyboard((key) => {
    const escape = key.name.toLowerCase() === "escape" || key.sequence === "\u001b"
    const openDialog = dialog()
    if (openDialog !== undefined) {
      if (escape || (openDialog.kind === "delete" && key.name.toLowerCase() === "n")) {
        key.preventDefault()
        key.stopPropagation()
        closeDialog()
      } else if (openDialog.kind === "delete" && (key.name === "return" || key.name.toLowerCase() === "y")) {
        key.preventDefault()
        key.stopPropagation()
        submitDialog()
      }
      return
    }
    if (taskCenterOpen()) {
      if (escape) {
        key.preventDefault()
        key.stopPropagation()
        setTaskCenterOpen(false)
      }
      return
    }
    if (exitPrompt()) {
      const choice = key.name.toLowerCase()
      if (escape || choice === "r") {
        key.preventDefault()
        key.stopPropagation()
        setExitPrompt(false)
      } else if (choice === "w") {
        key.preventDefault()
        key.stopPropagation()
        waitForTasks()
      } else if (choice === "c") {
        key.preventDefault()
        key.stopPropagation()
        cancelAndQuit()
      }
      return
    }
    const previewOpen = viewerOpen() || quickPreviewOpen()
    const copySelection = previewOpen && key.name.toLowerCase() === "c" &&
      ((key.ctrl && key.shift) || (key.meta && !key.ctrl && !key.shift))
    if (copySelection) {
      key.preventDefault()
      key.stopPropagation()
      copySelectedPreview()
      return
    }
    if (previewOpen && !key.ctrl && !key.meta && !key.shift && key.name.toLowerCase() === "y") {
      key.preventDefault()
      key.stopPropagation()
      const content = matchingPreview()?.content
      if (content === undefined) setNotice("Preview content unavailable")
      else copyPreview(content)
      return
    }
    if (quickPreviewOpen() && escape) {
      key.preventDefault()
      key.stopPropagation()
      setQuickPreviewOpen(false)
      return
    }
    if (viewerOpen() && escape) {
      key.preventDefault()
      key.stopPropagation()
      setViewerOpen(false)
      return
    }
    const result = keymap().handle(key)
    if (result.kind === "unhandled") return
    key.preventDefault()
    key.stopPropagation()
    setVisual(keymap().visual)
    if (result.kind === "pending") return
    if (result.kind === "visual-enter") {
      const snapshot = activeSnapshot()
      visualAnchor = snapshot?.cursor ?? -1
      if (snapshot === undefined || snapshot.cursor < 0) return
      sendCommand({ action: "toggle-selection" })
      return
    }
    if (result.kind === "visual-exit") return
    if (result.kind === "visual-move") {
      const snapshot = activeSnapshot()
      if (snapshot === undefined || snapshot.cursor < 0) return
      const target = snapshot.cursor + result.delta
      if (target < 0 || target >= snapshot.entries.length) return
      // Moving away from the anchor extends (move, then toggle the new row);
      // moving back towards it shrinks (untoggle the row being left).
      if (Math.abs(target - visualAnchor) < Math.abs(snapshot.cursor - visualAnchor)) {
        sendCommand({ action: "toggle-selection" })
        sendCommand({ action: "move", delta: result.delta })
      } else {
        sendCommand({ action: "move", delta: result.delta })
        sendCommand({ action: "toggle-selection" })
      }
      return
    }
    if (result.kind === "cancel") {
      quit()
      return
    }
    if (result.kind === "function") {
      dispatchFunction(result.action)
      return
    }
    if (result.kind === "search") {
      setDialog({ kind: "search", direction: result.direction })
      return
    }
    if (result.kind === "tab-index") {
      const workspace = props.workspace
      if (workspace === undefined || !canTabs()) {
        setNotice("Core pane tabs are unavailable")
        return
      }
      const pane = workspace.active_pane
      const tabs = workspace[`${pane}_tabs`] ?? []
      const tab = tabs[result.index]
      if (tab === undefined) {
        setNotice(`Tab ${result.index + 1} does not exist`)
        return
      }
      sendCommand({ action: "activate-tab", pane, tab_id: tab.id })
      return
    }
    if (result.kind === "command" && result.command.action === "focus-next") {
      setQuickPreviewOpen(false)
    }
    if (result.kind === "command" && result.command.action === "clear-selection" && (activeSnapshot()?.selection_count ?? 0) === 0) return
    if (result.command.action === "enter" && currentEntry()?.kind !== "directory" && currentEntry()?.resource_kind !== "archive") {
      if (props.onOpen !== undefined) openCurrentFile()
      else dispatchFunction("view")
      return
    }
    sendCommand(result.command)
  })

  const quickPreviewPane = () => quickPreviewOpen() && props.workspace !== undefined ? props.workspace.active_pane : undefined
  return <box width="100%" height="100%" flexDirection="column" backgroundColor={COLORS.crust} padding={1}>
    <Show when={exitPrompt()} fallback={<Show when={dialog()} fallback={
      <Show when={taskCenterOpen()} fallback={props.error !== undefined
        ? <ErrorPanel message={props.error} />
        : viewerOpen()
          ? <Viewer preview={matchingPreview()} onCopy={copyPreview} />
          : quickPreviewOpen() && !wide()
            ? <Viewer preview={matchingPreview()} onCopy={copyPreview} />
          : props.workspace !== undefined
            ? <Workspace
                workspace={props.workspace}
                wide={wide()}
                detailed={detailed()}
                showOwners={dimensions().width >= 220}
                iconMode={iconMode()}
                tabsEnabled={canTabs()}
                onSortDirection={(pane, key) => { sendCommand({ action: "sort-by", pane, key }) }}
                onSortCycle={(pane, delta) => { sendCommand({ action: "sort-cycle", pane, delta }) }}
                onSelect={(pane, index, toggle) => { sendCommand({ action: "select-entry", pane, index, toggle }) }}
                onActivateTab={(pane, tabId) => { sendCommand({ action: "activate-tab", pane, tab_id: tabId }) }}
                onCloseTab={(pane, tabId) => { sendCommand({ action: "close-tab", pane, tab_id: tabId }) }}
                onNewTab={(pane) => { sendCommand({ action: "new-tab", pane }) }}
                quickPreviewPane={quickPreviewPane()}
                quickPreview={matchingPreview()}
                onCopy={copyPreview}
              />
            : props.loading
              ? <LoadingPanel />
              : <ErrorPanel message="Core returned no workspace" />}>
        <TaskCenter actionTasks={props.actionTasks} resourceTasks={props.resourceTasks} onClose={() => setTaskCenterOpen(false)} onCommand={sendCommand} />
      </Show>
      }>
        {(state: () => DialogState) => <ActionDialog state={state()} onSubmit={submitDialog} onCancel={closeDialog} />}
      </Show>
    }>
      <ExitDialog taskCount={activeTaskCount()} stacked={dimensions().width < 110} onWait={waitForTasks} onCancel={cancelAndQuit} onReturn={() => setExitPrompt(false)} />
    </Show>
    <BottomBars workspace={props.workspace} tasks={props.tasks} actionTasks={props.actionTasks} resourceTasks={props.resourceTasks} compact={dimensions().width < 90} stacked={dimensions().width < 72} notice={props.commandError ?? notice()} visual={visual()} canView={currentEntry() !== undefined} canFileActions={canFileActions()} canResourceTasks={canResourceTasks()} iconMode={iconMode()} onAction={dispatchFunction} pathMode={pathMode()} homeDirectory={props.homeDirectory ?? process.env.HOME ?? process.env.USERPROFILE} onTogglePath={() => setPathMode((mode) => mode === "absolute" ? "home" : "absolute")} onCopyPath={copyStatusPath} onOpenTasks={() => setTaskCenterOpen((open) => !open)} />
  </box>
}
