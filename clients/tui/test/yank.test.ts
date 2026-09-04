import { expect, test } from "bun:test"

import type { SnapshotPayload, WorkspaceSnapshotPayload } from "../src/protocol.js"
import { resolvePutSource, yankFromSnapshot, type YankBuffer } from "../src/yank.js"

const entry = (overrides: Partial<SnapshotPayload["entries"][number]> = {}): SnapshotPayload["entries"][number] => ({
  name_display: "file.txt",
  name_bytes_hex: "66696c652e747874",
  path_display: "/tmp/file.txt",
  path_bytes_hex: "2f746d702f66696c652e747874",
  kind: "file",
  size_bytes: "12",
  mtime_unix_ms: "0",
  device: "11",
  inode: "21",
  ctime_unix_ns: "31",
  selected: false,
  hidden: false,
  ...overrides,
})

const snapshot = (overrides: Partial<SnapshotPayload> = {}): SnapshotPayload => ({
  cwd_display: "/tmp",
  cwd_bytes_hex: "2f746d70",
  generated_at_unix_ms: "0",
  snapshot_revision: "1",
  cwd_device: "10",
  cwd_inode: "20",
  cwd_ctime_unix_ns: "30",
  cursor: 0,
  entry_count: 1,
  selection_count: 0,
  filtered_count: 0,
  sort_key: "name",
  sort_descending: false,
  filter_active: false,
  entries: [entry()],
  ...overrides,
})

const workspace = (overrides: Partial<WorkspaceSnapshotPayload> = {}): WorkspaceSnapshotPayload => ({
  active_pane: "left",
  left: snapshot(),
  right: snapshot({ cwd_display: "/var", cwd_bytes_hex: "2f766172" }),
  ...overrides,
})

test("yanks the cursor entry when nothing is selected", () => {
  const buffer = yankFromSnapshot(snapshot())
  expect(buffer).toEqual({
    source_cwd_bytes_hex: "2f746d70",
    entries: [{ path_bytes_hex: "2f746d702f66696c652e747874", name_display: "file.txt", kind: "file" }],
  })
})

test("yanks the whole selection instead of only the cursor", () => {
  const buffer = yankFromSnapshot(snapshot({
    selection_count: 2,
    entries: [
      entry({ selected: true }),
      entry({ name_display: "b.txt", path_bytes_hex: "beef", selected: true }),
      entry({ name_display: "c.txt", path_bytes_hex: "c0ffee" }),
    ],
  }))
  expect(buffer?.entries.map((item) => item.name_display)).toEqual(["file.txt", "b.txt"])
})

test("returns undefined when there is nothing to yank", () => {
  expect(yankFromSnapshot(snapshot({ cursor: -1, entry_count: 0, entries: [] }))).toBeUndefined()
})

const buffer: YankBuffer = {
  source_cwd_bytes_hex: "2f746d70",
  entries: [{ path_bytes_hex: "2f746d702f66696c652e747874", name_display: "file.txt", kind: "file" }],
}

test("resolves put targets against the latest source pane snapshot", () => {
  const moved = workspace({
    left: snapshot({ snapshot_revision: "7", entries: [entry({ inode: "99", ctime_unix_ns: "88" })] }),
  })
  const resolved = resolvePutSource(buffer, moved)
  expect(resolved).toEqual({
    ok: true,
    source: {
      pane: "left",
      cwd_bytes_hex: "2f746d70",
      snapshot_revision: "7",
      cwd_device: "10",
      cwd_inode: "20",
      cwd_ctime_unix_ns: "30",
    },
    targets: [{ path_bytes_hex: "2f746d702f66696c652e747874", device: "11", inode: "99", ctime_unix_ns: "88", kind: "file" }],
  })
})

test("resolves a yank from the right pane after focus moved away", () => {
  const resolved = resolvePutSource(
    { source_cwd_bytes_hex: "2f766172", entries: buffer.entries.map((item) => ({ ...item, path_bytes_hex: "2f7661722f66696c652e747874" })) },
    workspace({ right: snapshot({ cwd_display: "/var", cwd_bytes_hex: "2f766172", entries: [entry({ path_bytes_hex: "2f7661722f66696c652e747874" })] }) }),
  )
  expect(resolved.ok).toBe(true)
  if (resolved.ok) expect(resolved.source.pane).toBe("right")
})

test("reports a source directory that is no longer visible in any pane", () => {
  const resolved = resolvePutSource(buffer, workspace({
    left: snapshot({ cwd_bytes_hex: "aaaa", entries: [] }),
  }))
  expect(resolved).toEqual({ ok: false, reason: "source-not-visible", missing: ["file.txt"] })
})

test("reports yanked entries that disappeared after a watcher refresh", () => {
  const resolved = resolvePutSource(buffer, workspace({ left: snapshot({ snapshot_revision: "9", entries: [] }) }))
  expect(resolved).toEqual({ ok: false, reason: "stale-targets", missing: ["file.txt"] })
})

test("rejects targets whose identity is incomplete in the latest snapshot", () => {
  const resolved = resolvePutSource(buffer, workspace({
    left: snapshot({ entries: [entry({ device: undefined })] }),
  }))
  expect(resolved).toEqual({ ok: false, reason: "unstable-identity", missing: ["file.txt"] })
})

test("rejects a source pane whose cwd identity is incomplete", () => {
  const resolved = resolvePutSource(buffer, workspace({
    left: snapshot({ cwd_device: undefined }),
  }))
  expect(resolved).toEqual({ ok: false, reason: "unstable-identity", missing: [] })
})
