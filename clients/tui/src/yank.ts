import type { CoreActionTarget } from "./core-client.js"
import type { EntryKind, PaneId, SnapshotPayload, WorkspaceSnapshotPayload } from "./protocol.js"

export interface YankEntry {
  readonly path_bytes_hex: string
  readonly name_display: string
  readonly kind: EntryKind
}

// A yank buffer only holds path references. Entry identities are re-resolved
// against the latest workspace on put, so watcher refreshes do not stale it.
export interface YankBuffer {
  readonly source_cwd_bytes_hex: string
  readonly entries: readonly YankEntry[]
}

export interface PutSourceContext {
  readonly pane: PaneId
  readonly cwd_bytes_hex: string
  readonly snapshot_revision: string
  readonly cwd_device: string
  readonly cwd_inode: string
  readonly cwd_ctime_unix_ns: string
}

export type PutSourceResolution =
  | Readonly<{ ok: true; source: PutSourceContext; targets: readonly CoreActionTarget[] }>
  | Readonly<{ ok: false; reason: "source-not-visible" | "stale-targets" | "unstable-identity"; missing: readonly string[] }>

export function yankFromSnapshot(snapshot: SnapshotPayload): YankBuffer | undefined {
  const entries = snapshot.selection_count !== 0
    ? snapshot.entries.filter((entry) => entry.selected)
    : snapshot.cursor < 0
      ? []
      : [snapshot.entries[snapshot.cursor]!]
  if (entries.length === 0) return undefined
  return {
    source_cwd_bytes_hex: snapshot.cwd_bytes_hex,
    entries: entries.map((entry) => ({
      path_bytes_hex: entry.path_bytes_hex,
      name_display: entry.name_display,
      kind: entry.kind,
    })),
  }
}

export function resolvePutSource(buffer: YankBuffer, workspace: WorkspaceSnapshotPayload): PutSourceResolution {
  const names = buffer.entries.map((entry) => entry.name_display)
  const pane = (["left", "right"] as const).find((candidate) => workspace[candidate].cwd_bytes_hex === buffer.source_cwd_bytes_hex)
  if (pane === undefined) return { ok: false, reason: "source-not-visible", missing: names }
  const snapshot: SnapshotPayload = workspace[pane]
  if (snapshot.cwd_device === undefined || snapshot.cwd_inode === undefined || snapshot.cwd_ctime_unix_ns === undefined || snapshot.snapshot_revision === "0") {
    return { ok: false, reason: "unstable-identity", missing: [] }
  }
  const targets: CoreActionTarget[] = []
  const missing: string[] = []
  let unstable = false
  for (const yanked of buffer.entries) {
    const entry = snapshot.entries.find((candidate) => candidate.path_bytes_hex === yanked.path_bytes_hex)
    if (entry === undefined) {
      missing.push(yanked.name_display)
      continue
    }
    if (entry.device === undefined || entry.inode === undefined || entry.ctime_unix_ns === undefined) {
      missing.push(yanked.name_display)
      unstable = true
      continue
    }
    targets.push({
      path_bytes_hex: entry.path_bytes_hex,
      device: entry.device,
      inode: entry.inode,
      ctime_unix_ns: entry.ctime_unix_ns,
      kind: entry.kind,
    })
  }
  if (missing.length !== 0) return { ok: false, reason: unstable ? "unstable-identity" : "stale-targets", missing }
  return {
    ok: true,
    source: {
      pane,
      cwd_bytes_hex: snapshot.cwd_bytes_hex,
      snapshot_revision: snapshot.snapshot_revision,
      cwd_device: snapshot.cwd_device,
      cwd_inode: snapshot.cwd_inode,
      cwd_ctime_unix_ns: snapshot.cwd_ctime_unix_ns,
    },
    targets,
  }
}
