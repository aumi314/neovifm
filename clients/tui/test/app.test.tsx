import { afterEach, expect, test } from "bun:test"
import { createSignal } from "solid-js"
import { testRender } from "@opentui/solid"
import { MouseButtons } from "@opentui/core/testing"

import { App, type AppProps } from "../src/app.js"
import type { ActionTaskPayload, SnapshotPayload, WorkspaceSnapshotPayload } from "../src/protocol.js"

const snapshot: SnapshotPayload = {
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
  entries: [
    {
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
      mode_octal: "100644",
      selected: false,
      hidden: false,
    },
  ],
}

const capabilities = ["workspace-sort-v1", "file-actions-v1", "pane-tabs-v1"] as const

let setup: Awaited<ReturnType<typeof testRender>> | undefined

const workspace: WorkspaceSnapshotPayload = {
  active_pane: "left",
  left_tabs: [
    { id: "1", cwd_display: "/tmp", active: true },
    { id: "2", cwd_display: "/Users/rex/project", active: false },
  ],
  right_tabs: [{ id: "3", cwd_display: "/var", active: true }],
  left: snapshot,
  right: {
    ...snapshot,
    cwd_display: "/var",
    cwd_bytes_hex: "2f766172",
    entries: [{ ...snapshot.entries[0]!, name_display: "right.txt" }],
  },
}

afterEach(() => {
  setup?.renderer.destroy()
  setup = undefined
})

test("renders two panes by default and degrades to the active pane when narrow", async () => {
  setup = await testRender(() => <App workspace={workspace} />, {
    width: 160,
    height: 20,
  })

  await setup.renderOnce()
  const wideFrame = setup.captureCharFrame()
  expect(wideFrame).toContain("file.txt")
  expect(wideFrame).toContain("right.txt")
  expect(wideFrame).toContain("●")
  expect(wideFrame).toContain("tmp")
  expect(wideFrame).toContain("project")
  expect(wideFrame).toContain("var")
  expect(wideFrame).not.toContain("LEFT")
  expect(wideFrame).not.toContain("RIGHT")
  expect(wideFrame).not.toContain("ACTIVE")
  expect(wideFrame).not.toContain("160x20")
  expect(wideFrame).toContain("-rw-r--r--")
  expect(wideFrame).toContain("1970-01-01")
  expect(wideFrame).toContain("Name ▲")
  expect(wideFrame).toContain("Permissions")
  expect(wideFrame).toContain("Size")
  expect(wideFrame).toContain("Created")
  expect(wideFrame).toContain("Modified")
  expect(wideFrame).toContain(" file.txt")
  expect(wideFrame).toContain("NORMAL")
  expect(wideFrame).toContain("")
  expect(wideFrame).toContain("")
  expect(wideFrame).toContain("")
  expect(wideFrame).toContain("F3 View")
  expect(wideFrame).toContain("F4 Edit")
  expect(wideFrame).toContain("F5 Copy")
  expect(wideFrame).toContain("F10 Quit")
  expect(wideFrame).not.toContain("READ ONLY")
  expect(wideFrame).not.toContain("| NORMAL |")

  setup.resize(60, 20)
  await setup.renderOnce()
  const compactFrame = setup.captureCharFrame()
  expect(compactFrame).not.toContain("60x20")
  expect(compactFrame).toContain("●")
  expect(compactFrame).not.toContain("right.txt")
  expect(compactFrame).toContain("F3")
  expect(compactFrame).toContain("F4")
  expect(compactFrame).toContain("F5")
  expect(compactFrame).toContain("F6")
  expect(compactFrame).toContain("F7")
  expect(compactFrame).toContain("F8")
  expect(compactFrame).toContain("F10")

  let sent: unknown
  setup?.renderer.destroy()
  setup = await testRender(() => <App workspace={workspace} onCommand={(command) => { sent = command }} />, { width: 60, height: 20 })
  await setup.renderOnce()
  setup.mockInput.pressTab()
  expect(sent).toEqual({ action: "focus-next" })
})

test("column headers and the function bar are clickable mouse targets", async () => {
  const sent: unknown[] = []
  let cancelled = false
  setup = await testRender(() => <App
    workspace={workspace}
    capabilities={capabilities}
    onCommand={(command) => { sent.push(command) }}
    onCancel={() => { cancelled = true }}
  />, { width: 180, height: 20 })
  await setup.renderOnce()

  const nameHeader = setup.renderer.root.findDescendantById("sort-left-name")
  expect(nameHeader).toBeDefined()
  await setup.mockMouse.click(nameHeader!.x, nameHeader!.y)
  expect(sent.at(-1)).toEqual({ action: "sort-by", pane: "left", key: "name" })
  await setup.mockMouse.click(nameHeader!.x, nameHeader!.y, MouseButtons.RIGHT)
  expect(sent.at(-1)).toEqual({ action: "sort-cycle", pane: "left", delta: 1 })

  const copyButton = setup.renderer.root.findDescendantById("function-copy")
  expect(copyButton).toBeDefined()
  await setup.mockMouse.click(copyButton!.x, copyButton!.y)
  expect(sent.at(-1)).toEqual({
    action: "copy",
    pane: "left",
    cwd_bytes_hex: snapshot.cwd_bytes_hex,
    snapshot_revision: snapshot.snapshot_revision,
    cwd_device: snapshot.cwd_device,
    cwd_inode: snapshot.cwd_inode,
    cwd_ctime_unix_ns: snapshot.cwd_ctime_unix_ns,
    destination_cwd_bytes_hex: workspace.right.cwd_bytes_hex,
    destination_snapshot_revision: workspace.right.snapshot_revision,
    destination_cwd_device: workspace.right.cwd_device,
    destination_cwd_inode: workspace.right.cwd_inode,
    destination_cwd_ctime_unix_ns: workspace.right.cwd_ctime_unix_ns,
    targets: [{
      path_bytes_hex: snapshot.entries[0]!.path_bytes_hex,
      device: snapshot.entries[0]!.device,
      inode: snapshot.entries[0]!.inode,
      ctime_unix_ns: snapshot.entries[0]!.ctime_unix_ns,
      kind: snapshot.entries[0]!.kind,
    }],
  })

  const moveButton = setup.renderer.root.findDescendantById("function-move")
  expect(moveButton).toBeDefined()
  await setup.mockMouse.click(moveButton!.x, moveButton!.y)
  expect(sent.at(-1)).toEqual({
    action: "move-files",
    pane: "left",
    cwd_bytes_hex: snapshot.cwd_bytes_hex,
    snapshot_revision: snapshot.snapshot_revision,
    cwd_device: snapshot.cwd_device,
    cwd_inode: snapshot.cwd_inode,
    cwd_ctime_unix_ns: snapshot.cwd_ctime_unix_ns,
    destination_cwd_bytes_hex: workspace.right.cwd_bytes_hex,
    destination_snapshot_revision: workspace.right.snapshot_revision,
    destination_cwd_device: workspace.right.cwd_device,
    destination_cwd_inode: workspace.right.cwd_inode,
    destination_cwd_ctime_unix_ns: workspace.right.cwd_ctime_unix_ns,
    targets: [{
      path_bytes_hex: snapshot.entries[0]!.path_bytes_hex,
      device: snapshot.entries[0]!.device,
      inode: snapshot.entries[0]!.inode,
      ctime_unix_ns: snapshot.entries[0]!.ctime_unix_ns,
      kind: snapshot.entries[0]!.kind,
    }],
  })

  const mkdirButton = setup.renderer.root.findDescendantById("function-mkdir")
  expect(mkdirButton).toBeDefined()
  await setup.mockMouse.click(mkdirButton!.x, mkdirButton!.y)
  await setup.renderOnce()
  expect(setup.captureCharFrame()).toContain("F7 MKDIR")
  const modalQuitButton = setup.renderer.root.findDescendantById("function-quit")
  expect(modalQuitButton).toBeDefined()
  await setup.mockMouse.click(modalQuitButton!.x, modalQuitButton!.y)
  expect(cancelled).toBe(false)
  await setup.mockInput.typeText("notes")
  setup.mockInput.pressEnter()
  await setup.renderOnce()
  expect(sent.at(-1)).toEqual({
    action: "mkdir",
    pane: "left",
    cwd_bytes_hex: snapshot.cwd_bytes_hex,
    snapshot_revision: snapshot.snapshot_revision,
    cwd_device: snapshot.cwd_device,
    cwd_inode: snapshot.cwd_inode,
    cwd_ctime_unix_ns: snapshot.cwd_ctime_unix_ns,
    name: "notes",
  })

  const deleteButton = setup.renderer.root.findDescendantById("function-delete")
  expect(deleteButton).toBeDefined()
  await setup.mockMouse.click(deleteButton!.x, deleteButton!.y)
  await setup.renderOnce()
  expect(setup.captureCharFrame()).toContain("F8 DELETE")
  setup.mockInput.pressKey("y")
  await setup.renderOnce()
  expect(sent.at(-1)).toEqual({
    action: "delete",
    pane: "left",
    cwd_bytes_hex: snapshot.cwd_bytes_hex,
    snapshot_revision: snapshot.snapshot_revision,
    cwd_device: snapshot.cwd_device,
    cwd_inode: snapshot.cwd_inode,
    cwd_ctime_unix_ns: snapshot.cwd_ctime_unix_ns,
    targets: [{
      path_bytes_hex: snapshot.entries[0]!.path_bytes_hex,
      device: snapshot.entries[0]!.device,
      inode: snapshot.entries[0]!.inode,
      ctime_unix_ns: snapshot.entries[0]!.ctime_unix_ns,
      kind: snapshot.entries[0]!.kind,
    }],
  })

  const quitButton = setup.renderer.root.findDescendantById("function-quit")
  expect(quitButton).toBeDefined()
  await setup.mockMouse.click(quitButton!.x, quitButton!.y)
  expect(cancelled).toBe(true)
})

test("opens the guarded F9 SSH dialog and sends a validated resource command", async () => {
  const sent: unknown[] = []
  setup = await testRender(() => <App
    workspace={workspace}
    capabilities={[...capabilities, "resource-tasks-v1"]}
    onCommand={(command) => { sent.push(command) }}
  />, { width: 120, height: 20 })
  await setup.renderOnce()

  const sshButton = setup.renderer.root.findDescendantById("function-mount-ssh")
  expect(sshButton).toBeDefined()
  await setup.mockMouse.click(sshButton!.x, sshButton!.y)
  await setup.renderOnce()
  expect(setup.captureCharFrame()).toContain("F9 SSH")
  expect(setup.renderer.root.findDescendantById("mount-ssh-input")).toBeDefined()

  await setup.mockInput.typeText("user@example.test:/srv/data")
  setup.mockInput.pressEnter()
  await setup.renderOnce()
  expect(sent.at(-1)).toEqual({
    action: "mount-ssh",
    pane: "left",
    remote: "user@example.test:/srv/data",
  })
})

test("opens directional search input and sends core-owned repeat commands", async () => {
  const sent: unknown[] = []
  const entries = [
    snapshot.entries[0]!,
    { ...snapshot.entries[0]!, name_display: "second.txt", path_bytes_hex: "2f746d702f7365636f6e642e747874" },
  ]
  const searchableWorkspace: WorkspaceSnapshotPayload = {
    ...workspace,
    left: { ...workspace.left, entry_count: entries.length, entries },
  }
  setup = await testRender(() => <App workspace={searchableWorkspace} onCommand={(command) => { sent.push(command) }} />, { width: 100, height: 20 })
  await setup.renderOnce()
  setup.mockInput.pressKey("/")
  await setup.renderOnce()
  expect(setup.renderer.root.findDescendantById("search-input")).toBeDefined()
  await setup.mockInput.typeText("second")
  setup.mockInput.pressEnter()
  await setup.renderOnce()
  expect(sent.at(-1)).toEqual({ action: "search", query: "second", direction: 1 })
  setup.mockInput.pressKey("n")
  await setup.renderOnce()
  expect(sent.at(-1)).toEqual({ action: "search-next", direction: 1 })
})

test("never allocates a third pane for preview or tasks", async () => {
  setup = await testRender(() => <App workspace={workspace} preview={{
    task_id: "1", generation: "2", pane: "left", kind: "text", state: "done",
    cwd_bytes_hex: snapshot.cwd_bytes_hex, path_bytes_hex: snapshot.entries[0]!.path_bytes_hex,
    content: "preview text", truncated: false,
  }} tasks={[{
    task_id: "1", generation: "2", pane: "left", kind: "text", state: "done",
    cwd_bytes_hex: snapshot.cwd_bytes_hex, path_bytes_hex: snapshot.entries[0]!.path_bytes_hex,
  }]} />, { width: 120, height: 20 })

  await setup.renderOnce()
  expect(setup.captureCharFrame()).not.toContain("preview text")
  expect(setup.captureCharFrame()).not.toContain("done:text")
  expect(setup.captureCharFrame()).toContain("file.txt")
  expect(setup.captureCharFrame()).toContain("right.txt")

  setup.resize(60, 20)
  await setup.renderOnce()
  expect(setup.captureCharFrame()).not.toContain("preview text")
})

test("opens F3 preview as a full workspace viewer instead of a third pane", async () => {
  setup = await testRender(() => <App workspace={workspace} preview={{
    task_id: "1", generation: "2", pane: "left", kind: "text", state: "done",
    cwd_bytes_hex: snapshot.cwd_bytes_hex, path_bytes_hex: snapshot.entries[0]!.path_bytes_hex,
    content: "preview body", truncated: false,
  }} />, { width: 100, height: 20 })

  await setup.renderOnce()
  expect(setup.captureCharFrame()).not.toContain("preview body")
  setup.mockInput.pressKey("F3")
  await setup.renderOnce()
  expect(setup.captureCharFrame()).toContain("F3 VIEW")
  expect(setup.captureCharFrame()).toContain("preview body")
  setup.mockInput.pressKey("F3")
  await setup.renderOnce()
  expect(setup.captureCharFrame()).not.toContain("preview body")
})

test("keeps preview text selectable and copies the original Unicode content", async () => {
  const copied: string[] = []
  const content = "\u7b2c\u4e00\u884c\n\u4e2d\u6587\tsecond"
  setup = await testRender(() => <App workspace={workspace} preview={{
    task_id: "1", generation: "2", pane: "left", kind: "text", state: "done",
    cwd_bytes_hex: snapshot.cwd_bytes_hex, path_bytes_hex: snapshot.entries[0]!.path_bytes_hex,
    content, truncated: false,
  }} onCopyText={(text) => { copied.push(text) }} />, { width: 100, height: 20 })

  await setup.renderOnce()
  setup.mockInput.pressKey("F3")
  await setup.renderOnce()
  const previewContent = setup.renderer.root.findDescendantById("preview-content") as { selectable?: boolean } | undefined
  expect(previewContent?.selectable).toBe(true)
  const copyButton = setup.renderer.root.findDescendantById("preview-copy")
  expect(copyButton).toBeDefined()
  await setup.mockMouse.click(copyButton!.x, copyButton!.y)
  await Bun.sleep(0)
  await setup.renderOnce()
  expect(copied).toEqual([content])
  expect(setup.captureCharFrame()).toContain("Preview copied")
  setup.mockInput.pressKey("y")
  await Bun.sleep(0)
  expect(copied).toEqual([content, content])
})

test("uses Space for an ephemeral opposite-pane preview while Tab still changes focus", async () => {
  const sent: unknown[] = []
  setup = await testRender(() => <App workspace={workspace} onCommand={(command) => { sent.push(command) }} preview={{
    task_id: "1", generation: "2", pane: "left", target_pane: "right", kind: "text", state: "done",
    cwd_bytes_hex: snapshot.cwd_bytes_hex, path_bytes_hex: snapshot.entries[0]!.path_bytes_hex,
    content: "opposite pane preview", truncated: false,
  }} />, { width: 120, height: 20 })

  await setup.renderOnce()
  setup.mockInput.pressKey(" ")
  await setup.renderOnce()
  expect(sent).toContainEqual(expect.objectContaining({
    action: "preview",
    pane: "left",
    target_pane: "right",
    snapshot_revision: "1",
    path_bytes_hex: snapshot.entries[0]!.path_bytes_hex,
  }))
  const quickFrame = setup.captureCharFrame()
  expect(quickFrame).toContain("SPACE QUICK VIEW")
  expect(quickFrame).toContain("opposite pane preview")
  expect(quickFrame).not.toContain("right.txt")

  setup.mockInput.pressTab()
  await setup.renderOnce()
  expect(sent.at(-1)).toEqual({ action: "focus-next" })
  expect(setup.captureCharFrame()).not.toContain("opposite pane preview")
})

test("copies content from the wide Space quick preview", async () => {
  const copied: string[] = []
  const content = "quick\n\u9884\u89c8"
  setup = await testRender(() => <App workspace={workspace} onCommand={() => true} onCopyText={(text) => { copied.push(text) }} preview={{
    task_id: "1", generation: "2", pane: "left", target_pane: "right", kind: "text", state: "done",
    cwd_bytes_hex: snapshot.cwd_bytes_hex, path_bytes_hex: snapshot.entries[0]!.path_bytes_hex,
    content, truncated: false,
  }} />, { width: 120, height: 20 })

  await setup.renderOnce()
  setup.mockInput.pressKey(" ")
  await setup.renderOnce()
  const copyButton = setup.renderer.root.findDescendantById("preview-copy")
  expect(copyButton).toBeDefined()
  await setup.mockMouse.click(copyButton!.x, copyButton!.y)
  await Bun.sleep(0)
  expect(copied).toEqual([content])
})

test("falls back to the F3 full-screen viewer for Space in a narrow terminal", async () => {
  setup = await testRender(() => <App workspace={workspace} onCommand={() => true} preview={{
    task_id: "1", generation: "2", pane: "left", target_pane: "right", kind: "text", state: "done",
    cwd_bytes_hex: snapshot.cwd_bytes_hex, path_bytes_hex: snapshot.entries[0]!.path_bytes_hex,
    content: "narrow opposite pane preview", truncated: false,
  }} />, { width: 60, height: 20 })

  await setup.renderOnce()
  setup.mockInput.pressKey(" ")
  await setup.renderOnce()
  const frame = setup.captureCharFrame()
  expect(frame).toContain("F3 VIEW")
  expect(frame).toContain("narrow opposite pane preview")
  expect(frame).not.toContain("SPACE QUICK VIEW")
  expect(frame).not.toContain("right.txt")
})

test("opens a file with l through the same preview path as F3", async () => {
  setup = await testRender(() => <App workspace={workspace} preview={{
    task_id: "1", generation: "2", pane: "left", kind: "text", state: "done",
    cwd_bytes_hex: snapshot.cwd_bytes_hex, path_bytes_hex: snapshot.entries[0]!.path_bytes_hex,
    content: "opened with l", truncated: false,
  }} />, { width: 100, height: 20 })

  await setup.renderOnce()
  setup.mockInput.pressKey("l")
  await setup.renderOnce()
  expect(setup.captureCharFrame()).toContain("F3 VIEW")
  expect(setup.captureCharFrame()).toContain("opened with l")
})

test("routes l on a regular file to the external opener when configured", async () => {
  let openedPath: string | undefined
  setup = await testRender(() => <App workspace={workspace} onOpen={(path) => { openedPath = path }} />, { width: 100, height: 20 })
  await setup.renderOnce()
  setup.mockInput.pressKey("l")
  await setup.renderOnce()
  expect(openedPath).toBe("/tmp/file.txt")
})

test("routes regular-file Enter through the core open resolver when advertised", async () => {
  let sent: unknown
  setup = await testRender(() => <App
    workspace={workspace}
    capabilities={[...capabilities, "open-v1"]}
    onCommand={(command) => { sent = command }}
    onOpen={() => { throw new Error("client fallback must not run") }}
  />, { width: 100, height: 20 })
  await setup.renderOnce()
  setup.mockInput.pressKey("l")
  await setup.renderOnce()
  expect(sent).toEqual({
    action: "open",
    intent: "open",
    pane: "left",
    cwd_bytes_hex: snapshot.cwd_bytes_hex,
    snapshot_revision: snapshot.snapshot_revision,
    cwd_device: snapshot.cwd_device,
    cwd_inode: snapshot.cwd_inode,
    cwd_ctime_unix_ns: snapshot.cwd_ctime_unix_ns,
    path_bytes_hex: snapshot.entries[0]!.path_bytes_hex,
    device: snapshot.entries[0]!.device,
    inode: snapshot.entries[0]!.inode,
    ctime_unix_ns: snapshot.entries[0]!.ctime_unix_ns,
  })
})

test("routes an archive Enter through the core resource command", async () => {
  let sent: unknown
  const archiveWorkspace: WorkspaceSnapshotPayload = {
    ...workspace,
    left: {
      ...workspace.left,
      entries: [{ ...workspace.left.entries[0]!, name_display: "bundle.zip", resource_kind: "archive" }],
    },
  }
  setup = await testRender(() => <App
    workspace={archiveWorkspace}
    capabilities={[...capabilities, "open-v1"]}
    onCommand={(command) => { sent = command }}
    onOpen={() => { throw new Error("archive must not use the platform opener") }}
  />, { width: 100, height: 20 })
  await setup.renderOnce()
  setup.mockInput.pressKey("l")
  await setup.renderOnce()
  expect(sent).toEqual({ action: "enter" })
})

test("opens a loading viewer instead of rendering a stale preview from another pane or cursor identity", async () => {
  const staleWorkspace: WorkspaceSnapshotPayload = {
    ...workspace,
    active_pane: "right",
    right: { ...workspace.right, entries: [{ ...workspace.right.entries[0]!, path_bytes_hex: "2f7661722f6f74686572" }] },
  }
  setup = await testRender(() => <App workspace={staleWorkspace} preview={{
    task_id: "1", generation: "2", pane: "left", kind: "text", state: "done",
    cwd_bytes_hex: snapshot.cwd_bytes_hex, path_bytes_hex: snapshot.entries[0]!.path_bytes_hex,
    content: "stale preview", truncated: false,
  }} />, { width: 100, height: 20 })
  await setup.renderOnce()
  setup.mockInput.pressKey("F3")
  await setup.renderOnce()
  expect(setup.captureCharFrame()).not.toContain("stale preview")
  expect(setup.captureCharFrame()).toContain("F3 VIEW")
  expect(setup.captureCharFrame()).toContain("Loading preview")
})

test("keeps the C-owned cursor visible after it moves below the first viewport", async () => {
  const entries = Array.from({ length: 40 }, (_, index) => ({
    ...snapshot.entries[0]!,
    name_display: `entry-${String(index).padStart(2, "0")}`,
    path_bytes_hex: `2f746d702f${index.toString(16).padStart(2, "0")}`,
  }))
  const deepWorkspace: WorkspaceSnapshotPayload = {
    ...workspace,
    left: { ...workspace.left, cursor: 35, entry_count: entries.length, entries },
  }
  setup = await testRender(() => <App workspace={deepWorkspace} />, { width: 100, height: 12 })
  await setup.renderOnce()
  await Bun.sleep(5)
  await setup.renderOnce()
  const list = setup.renderer.root.findDescendantById("entries-left") as { scrollTop?: number } | undefined
  expect(list).toBeDefined()
  expect(list?.scrollTop ?? 0).toBeGreaterThan(0)
  expect(setup.captureCharFrame()).toContain("entry-35")
})

test("restores the parent directory scroll position after entering a child", async () => {
  const parentEntries = Array.from({ length: 40 }, (_, index) => ({
    ...snapshot.entries[0]!,
    name_display: index === 35 ? "target-dir" : `entry-${String(index).padStart(2, "0")}`,
    path_bytes_hex: `2f746d702f${index.toString(16).padStart(2, "0")}`,
    kind: index === 35 ? "directory" as const : "file" as const,
  }))
  const parentWorkspace: WorkspaceSnapshotPayload = {
    ...workspace,
    left: {
      ...workspace.left,
      cwd_display: "/tmp",
      cwd_bytes_hex: "2f746d70",
      cursor: 39,
      entry_count: parentEntries.length,
      entries: parentEntries,
    },
  }
  const childWorkspace: WorkspaceSnapshotPayload = {
    ...parentWorkspace,
    left: {
      ...snapshot,
      cwd_display: "/tmp/target-dir",
      cwd_bytes_hex: "2f746d702f7461726765742d646972",
      cursor: 0,
      entry_count: 1,
      entries: [{ ...snapshot.entries[0]!, name_display: "inside.txt", path_bytes_hex: "2f746d702f7461726765742d6469722f696e736964652e747874" }],
    },
  }
  const [current, setCurrent] = createSignal(parentWorkspace)
  setup = await testRender(() => <App workspace={current()} />, { width: 100, height: 12 })
  await setup.renderOnce()
  await Bun.sleep(5)
  await setup.renderOnce()
  setCurrent({ ...parentWorkspace, left: { ...parentWorkspace.left, cursor: 35 } })
  await setup.renderOnce()
  await Bun.sleep(5)
  await setup.renderOnce()
  const list = setup.renderer.root.findDescendantById("entries-left") as { scrollTop?: number } | undefined
  expect(list).toBeDefined()
  const parentScrollTop = list?.scrollTop ?? 0
  expect(parentScrollTop).toBeGreaterThan(0)

  setCurrent(childWorkspace)
  await setup.renderOnce()
  await Bun.sleep(5)
  await setup.renderOnce()
  setCurrent({ ...parentWorkspace, left: { ...parentWorkspace.left, cursor: 35 } })
  await setup.renderOnce()
  await Bun.sleep(5)
  await setup.renderOnce()

  expect(list?.scrollTop ?? 0).toBe(parentScrollTop)
  expect(setup.captureCharFrame()).toContain("target-dir")
})

test("mouse clicks provide F3 and F4 fallbacks when the host captures function keys", async () => {
  let editedPath: string | undefined
  setup = await testRender(() => <App
    workspace={workspace}
    preview={{
      task_id: "1", generation: "2", pane: "left", kind: "text", state: "done",
      cwd_bytes_hex: snapshot.cwd_bytes_hex, path_bytes_hex: snapshot.entries[0]!.path_bytes_hex,
      content: "mouse preview", truncated: false,
    }}
    onEdit={(path) => { editedPath = path }}
  />, { width: 100, height: 20 })
  await setup.renderOnce()

  const viewButton = setup.renderer.root.findDescendantById("function-view")
  expect(viewButton).toBeDefined()
  await setup.mockMouse.click(viewButton!.x, viewButton!.y)
  await setup.renderOnce()
  expect(setup.captureCharFrame()).toContain("mouse preview")
  await setup.mockMouse.click(viewButton!.x, viewButton!.y)
  await setup.renderOnce()

  const editButton = setup.renderer.root.findDescendantById("function-edit")
  expect(editButton).toBeDefined()
  await setup.mockMouse.click(editButton!.x, editButton!.y)
  await setup.renderOnce()
  expect(editedPath).toBe("/tmp/file.txt")
})

test("keeps F3 clickable while the current preview is still loading", async () => {
  setup = await testRender(() => <App workspace={workspace} preview={{
    task_id: "1", generation: "2", pane: "left", kind: "text", state: "running",
    cwd_bytes_hex: snapshot.cwd_bytes_hex, path_bytes_hex: snapshot.entries[0]!.path_bytes_hex,
    truncated: false,
  }} />, { width: 100, height: 20 })
  await setup.renderOnce()

  const viewButton = setup.renderer.root.findDescendantById("function-view")
  expect(viewButton).toBeDefined()
  await setup.mockMouse.click(viewButton!.x, viewButton!.y)
  await setup.renderOnce()
  const frame = setup.captureCharFrame()
  expect(frame).toContain("F3 VIEW")
  expect(frame).toContain("text · running · generation 2")
  expect(frame).toContain("Loading preview...")
})

test("routes F4 edit through the shared action service", async () => {
  let editedPath: string | undefined
  const displayOnlyWorkspace: WorkspaceSnapshotPayload = {
    ...workspace,
    left: {
      ...workspace.left,
      entries: [{ ...workspace.left.entries[0]!, path_display: "/display-only" }],
    },
  }
  setup = await testRender(() => <App workspace={displayOnlyWorkspace} onEdit={(path) => { editedPath = path }} />, { width: 100, height: 20 })
  await setup.renderOnce()
  setup.mockInput.pressKey("F4")
  await setup.renderOnce()
  expect(editedPath).toBe("/tmp/file.txt")
})

test("refuses to pass a lossy non-UTF-8 display path to an editor", async () => {
  let edited = false
  const invalidIdentityWorkspace: WorkspaceSnapshotPayload = {
    ...workspace,
    left: {
      ...workspace.left,
      entries: [{ ...workspace.left.entries[0]!, path_bytes_hex: "ff" }],
    },
  }
  setup = await testRender(() => <App workspace={invalidIdentityWorkspace} onEdit={() => { edited = true }} />, { width: 100, height: 20 })
  await setup.renderOnce()
  setup.mockInput.pressKey("F4")
  await setup.renderOnce()
  expect(edited).toBe(false)
  expect(setup.captureCharFrame()).toContain("non-UTF-8 path")
})

test("reports a closed core channel instead of pretending a click succeeded", async () => {
  setup = await testRender(() => <App workspace={workspace} capabilities={capabilities} onCommand={() => false} />, { width: 100, height: 20 })
  await setup.renderOnce()
  const copyButton = setup.renderer.root.findDescendantById("function-copy")
  expect(copyButton).toBeDefined()
  await setup.mockMouse.click(copyButton!.x, copyButton!.y)
  await setup.renderOnce()
  expect(setup.captureCharFrame()).toContain("Core command channel is unavailable")
})

test("keeps file actions available while an earlier action is running", async () => {
  const sent: unknown[] = []
  const runningAction: ActionTaskPayload = {
    task_id: "4",
    command_sequence: 3,
    pane: "left",
    action: "copy",
    state: "running",
    completed_count: 0,
    total_count: 1,
    partial: false,
    retryable: false,
  }
  setup = await testRender(() => <App
    workspace={workspace}
    capabilities={capabilities}
    actionTasks={[runningAction]}
    onCommand={(command) => { sent.push(command) }}
  />, { width: 100, height: 20 })
  await setup.renderOnce()
  const copyButton = setup.renderer.root.findDescendantById("function-copy")
  expect(copyButton).toBeDefined()
  await setup.mockMouse.click(copyButton!.x, copyButton!.y)
  expect(sent.at(-1)).toMatchObject({ action: "copy", pane: "left" })
})

test("opens a clickable task center with queue and history", async () => {
  const tasks: ActionTaskPayload[] = [
    {
      task_id: "4", command_sequence: 3, pane: "left", action: "copy", state: "running",
      completed_count: 1, total_count: 3, partial: false, retryable: false,
    },
    {
      task_id: "3", command_sequence: 2, pane: "left", action: "move", state: "done",
      completed_count: 1, total_count: 1, partial: false, retryable: false, undo_available: true,
    },
  ]
  const sent: unknown[] = []
  setup = await testRender(() => <App workspace={workspace} actionTasks={tasks} onCommand={(command) => { sent.push(command) }} />, { width: 100, height: 20 })
  await setup.renderOnce()
  const entry = setup.renderer.root.findDescendantById("tasks-entry")
  expect(entry).toBeDefined()
  await setup.mockMouse.click(entry!.x, entry!.y)
  await setup.renderOnce()
  const frame = setup.captureCharFrame()
  expect(frame).toContain("TASK CENTER")
  expect(frame).toContain("QUEUE (1)")
  expect(frame).toContain("HISTORY (1)")
  expect(frame).toContain("copy #4 running 1/3")
  expect(frame).not.toContain("move #3 done 1/1")
  const historyTab = setup.renderer.root.findDescendantById("task-history-tab")
  expect(historyTab).toBeDefined()
  await setup.mockMouse.click(historyTab!.x, historyTab!.y)
  await setup.renderOnce()
  expect(setup.captureCharFrame()).toContain("move #3 done 1/1")
  const historyEntry = setup.renderer.root.findDescendantById("task-row-3")
  expect(historyEntry).toBeDefined()
  await setup.mockMouse.click(historyEntry!.x, historyEntry!.y)
  await setup.renderOnce()
  expect(setup.captureCharFrame()).toContain("Undo available")
  const cancelEntry = setup.renderer.root.findDescendantById("task-row-4")
  expect(cancelEntry).toBeUndefined()
  const queueTab = setup.renderer.root.findDescendantById("task-queue-tab")
  expect(queueTab).toBeDefined()
  await setup.mockMouse.click(queueTab!.x, queueTab!.y)
  await setup.renderOnce()
  const queueEntry = setup.renderer.root.findDescendantById("task-row-4")
  expect(queueEntry).toBeDefined()
  await setup.mockMouse.click(queueEntry!.x, queueEntry!.y)
  expect(sent.at(-1)).toEqual({ action: "cancel-action", task_id: "4" })
  const closeEntry = setup.renderer.root.findDescendantById("tasks-entry")
  expect(closeEntry).toBeDefined()
  await setup.mockMouse.click(closeEntry!.x, closeEntry!.y)
  await setup.renderOnce()
  expect(setup.captureCharFrame()).not.toContain("TASK CENTER")
})

test("offers wait, cancel, and return choices before quitting with active tasks", async () => {
  const [tasks, setTasks] = createSignal<readonly ActionTaskPayload[]>([{
    task_id: "21", command_sequence: 8, pane: "left", action: "copy", state: "running",
    completed_count: 0, total_count: 1, partial: false, retryable: false,
  }])
  const sent: unknown[] = []
  let cancelled = false
  setup = await testRender(() => <App
    workspace={workspace}
    actionTasks={tasks()}
    onCommand={(command) => { sent.push(command) }}
    onCancel={() => { cancelled = true }}
  />, { width: 100, height: 20 })
  await setup.renderOnce()
  const quitButton = setup.renderer.root.findDescendantById("function-quit")
  expect(quitButton).toBeDefined()
  await setup.mockMouse.click(quitButton!.x, quitButton!.y)
  await setup.renderOnce()
  expect(setup.captureCharFrame()).toContain("TASKS STILL RUNNING")
  expect(setup.renderer.root.findDescendantById("exit-wait")).toBeDefined()
  expect(cancelled).toBe(false)

  const returnButton = setup.renderer.root.findDescendantById("exit-return")
  expect(returnButton).toBeDefined()
  await setup.mockMouse.click(returnButton!.x, returnButton!.y)
  await setup.renderOnce()
  expect(setup.captureCharFrame()).not.toContain("TASKS STILL RUNNING")

  await setup.mockMouse.click(quitButton!.x, quitButton!.y)
  await setup.renderOnce()
  const cancelButton = setup.renderer.root.findDescendantById("exit-cancel")
  expect(cancelButton).toBeDefined()
  await setup.mockMouse.click(cancelButton!.x, cancelButton!.y)
  expect(sent).toEqual([{ action: "cancel-action", task_id: "21" }])
  expect(cancelled).toBe(false)
  setTasks([])
  await Bun.sleep(20)
  expect(cancelled).toBe(true)
})

test("stacks exit choices in a narrow terminal", async () => {
  setup = await testRender(() => <App workspace={workspace} actionTasks={[{
    task_id: "22", command_sequence: 9, pane: "left", action: "copy", state: "running",
    completed_count: 0, total_count: 1, partial: false, retryable: false,
  }]} onCancel={() => undefined} />, { width: 60, height: 20 })
  await setup.renderOnce()
  const quitButton = setup.renderer.root.findDescendantById("function-quit")
  expect(quitButton).toBeDefined()
  await setup.mockMouse.click(quitButton!.x, quitButton!.y)
  await setup.renderOnce()
  const frame = setup.captureCharFrame()
  expect(frame).toContain("TASKS STILL RUNNING")
  expect(frame).toContain("[Wait for tasks]")
  expect(frame).toContain("[Cancel and exit]")
  expect(frame).toContain("[Return]")
})

test("shows terminal task details and sends only core-owned safe retry", async () => {
  const tasks: ActionTaskPayload[] = [
    {
      task_id: "7", command_sequence: 6, pane: "left", action: "copy", state: "failed",
      completed_count: 1, total_count: 2, failed_index: 1, partial: true,
      error_code: "destination-exists", os_error: 17, retryable: true,
      source_path_bytes_hex: "2f746d702f736f75726365",
      destination_path_bytes_hex: "2f746d702f64657374696e6174696f6e",
      current_path_bytes_hex: "2f746d702f736f757263652f6e6f7465",
      bytes_known: true, bytes_completed: "12", bytes_total: "42",
      started_at_unix_ms: "1700000000000", finished_at_unix_ms: "1700000000123",
    },
    {
      task_id: "8", command_sequence: 7, pane: "right", action: "delete", state: "cancelled",
      completed_count: 0, total_count: 1, failed_index: 0, partial: false,
      error_code: "cancelled", retryable: false,
    },
  ]
  const sent: unknown[] = []
  setup = await testRender(() => <App workspace={workspace} actionTasks={tasks} onCommand={(command) => { sent.push(command) }} />, { width: 100, height: 24 })
  await setup.renderOnce()

  const entry = setup.renderer.root.findDescendantById("tasks-entry")
  expect(entry).toBeDefined()
  await setup.mockMouse.click(entry!.x, entry!.y)
  await setup.renderOnce()
  const historyTab = setup.renderer.root.findDescendantById("task-history-tab")
  expect(historyTab).toBeDefined()
  await setup.mockMouse.click(historyTab!.x, historyTab!.y)
  await setup.renderOnce()

  const failedRow = setup.renderer.root.findDescendantById("task-row-7")
  expect(failedRow).toBeDefined()
  await setup.mockMouse.click(failedRow!.x, failedRow!.y)
  await setup.renderOnce()
  let frame = setup.captureCharFrame()
  expect(frame).toContain("TASK DETAILS")
  expect(frame).toContain("Task 7")
  expect(frame).toContain("copy · failed · left")
  expect(frame).toContain("Progress 1/2")
  expect(frame).toContain("Time 2023-11-14")
  expect(frame).toContain("12 B")
  expect(frame).toContain("/tmp/source")
  expect(frame).toContain("/tmp/destination")
  expect(frame).toContain("Failed item 2")
  expect(frame).toContain("destination-exists")
  expect(frame).toContain("Retry task")

  const retry = setup.renderer.root.findDescendantById("task-retry-7")
  expect(retry).toBeDefined()
  await setup.mockMouse.click(retry!.x, retry!.y)
  expect(sent).toEqual([{ action: "retry-action", task_id: "7" }])

  const cancelledRow = setup.renderer.root.findDescendantById("task-row-8")
  expect(cancelledRow).toBeDefined()
  await setup.mockMouse.click(cancelledRow!.x, cancelledRow!.y)
  await setup.renderOnce()
  frame = setup.captureCharFrame()
  expect(frame).toContain("Task 8")
  expect(frame).toContain("delete · cancelled · right")
  expect(frame).toContain("Retry unavailable")
})

test("renders a core error without a snapshot", async () => {
  setup = await testRender(() => <App error="cannot scan directory" />, {
    width: 60,
    height: 12,
  })

  await setup.renderOnce()
  expect(setup.captureCharFrame()).toContain("cannot scan directory")
})

test("renders a cancellable loading state before a snapshot arrives", async () => {
  setup = await testRender(() => <App loading />, {
    width: 60,
    height: 12,
  })

  await setup.renderOnce()
  expect(setup.captureCharFrame()).toContain("CONNECTING TO CORE")
})

test("reacts to the root app-props accessor used by the renderer", async () => {
  const [props, setProps] = createSignal<AppProps>({ loading: true })
  setup = await testRender(() => <App {...props()} />, {
    width: 100,
    height: 20,
  })

  await setup.renderOnce()
  expect(setup.captureCharFrame()).toContain("CONNECTING TO CORE")

  setProps({ workspace })
  await setup.renderOnce()
  const frame = setup.captureCharFrame()
  expect(frame).toContain("file.txt")
  expect(frame).not.toContain("CONNECTING TO CORE")
})

test("uses a refreshed core workspace as the active-pane source of truth", async () => {
  const [props, setProps] = createSignal<AppProps>({ workspace })
  setup = await testRender(() => <App {...props()} />, { width: 60, height: 20 })

  await setup.renderOnce()
  setProps({ workspace: { ...workspace, active_pane: "right", left: { ...workspace.left }, right: { ...workspace.right } } })
  await setup.renderOnce()
  const frame = setup.captureCharFrame()
  expect(frame).toContain("●")
  expect(frame).not.toContain("RIGHT ACTIVE")
})

test("cycles the compact metadata column between size, time, and permissions", async () => {
  const [props, setProps] = createSignal<AppProps>({ workspace })
  setup = await testRender(() => <App {...props()} />, { width: 100, height: 20 })

  await setup.renderOnce()
  expect(setup.captureCharFrame()).toContain("Size")
  setProps({ workspace: { ...workspace, left: { ...workspace.left, sort_key: "mtime" } } })
  await setup.renderOnce()
  expect(setup.captureCharFrame()).toContain("Modified ▲")
  expect(setup.captureCharFrame()).toContain("1970-01-01")
  setProps({ workspace: { ...workspace, left: { ...workspace.left, sort_key: "mode" } } })
  await setup.renderOnce()
  expect(setup.captureCharFrame()).toContain("Permissions ▲")
  expect(setup.captureCharFrame()).toContain("-rw-r--r--")
})

test("shows owner and group columns only when the wide layout has metadata", async () => {
  const ownerWorkspace: WorkspaceSnapshotPayload = {
    ...workspace,
    left: { ...workspace.left, entries: [{ ...workspace.left.entries[0]!, owner_display: "rex", group_display: "staff" }] },
    right: { ...workspace.right, entries: [{ ...workspace.right.entries[0]!, owner_display: "root", group_display: "wheel" }] },
  }
  setup = await testRender(() => <App workspace={ownerWorkspace} />, { width: 240, height: 20 })
  await setup.renderOnce()
  const frame = setup.captureCharFrame()
  expect(frame).toContain("Owner")
  expect(frame).toContain("Group")
  expect(frame).toContain("rex")
  expect(frame).toContain("staff")
})

test("keeps the workspace operable without Nerd Font or powerline glyphs", async () => {
  setup = await testRender(() => <App workspace={workspace} iconMode="ascii" />, { width: 100, height: 20 })
  await setup.renderOnce()
  const frame = setup.captureCharFrame()
  expect(frame).toContain("- file.txt")
  expect(frame).toContain("NORMAL")
  expect(frame).not.toContain("")
  expect(frame).not.toContain("")
  expect(frame).not.toContain("")
  expect(frame).not.toContain("")
  expect(frame).toContain("[F3 View]")
})

test("activates, closes, and creates pane tabs with mouse buttons", async () => {
  const sent: unknown[] = []
  setup = await testRender(() => <App workspace={workspace} capabilities={capabilities} onCommand={(command) => { sent.push(command) }} />, { width: 120, height: 20 })
  await setup.renderOnce()

  const secondTab = setup.renderer.root.findDescendantById("tab-left-2")
  expect(secondTab).toBeDefined()
  await setup.mockMouse.click(secondTab!.x, secondTab!.y)
  expect(sent.at(-1)).toEqual({ action: "activate-tab", pane: "left", tab_id: "2" })
  await setup.mockMouse.click(secondTab!.x, secondTab!.y, MouseButtons.RIGHT)
  expect(sent.at(-1)).toEqual({ action: "close-tab", pane: "left", tab_id: "2" })

  const newTab = setup.renderer.root.findDescendantById("tab-left-new")
  expect(newTab).toBeDefined()
  await setup.mockMouse.click(newTab!.x, newTab!.y)
  expect(sent.at(-1)).toEqual({ action: "new-tab", pane: "left" })
})

test("keeps all eight tab targets and the new-tab button inside an 80-column pane", async () => {
  const leftTabs = Array.from({ length: 8 }, (_, index) => ({
    id: String(index + 10),
    cwd_display: `/tmp/project-with-a-long-name-${index + 1}`,
    active: index === 3,
  }))
  const crowdedWorkspace: WorkspaceSnapshotPayload = { ...workspace, left_tabs: leftTabs }
  setup = await testRender(() => <App workspace={crowdedWorkspace} capabilities={capabilities} />, { width: 80, height: 20 })
  await setup.renderOnce()

  for (const tab of leftTabs) {
    expect(setup.renderer.root.findDescendantById(`tab-left-${tab.id}`)).toBeDefined()
  }
  const newTab = setup.renderer.root.findDescendantById("tab-left-new")
  const firstRightTab = setup.renderer.root.findDescendantById("tab-right-3")
  expect(newTab).toBeDefined()
  expect(firstRightTab).toBeDefined()
  expect(newTab!.x + newTab!.width).toBeLessThan(firstRightTab!.x)
  expect(setup.captureCharFrame()).toContain("4 proje…")
})

test("selects file rows with left click and toggles batch selection with right click", async () => {
  const sent: unknown[] = []
  const entries = [
    snapshot.entries[0]!,
    { ...snapshot.entries[0]!, name_display: "second.txt", path_bytes_hex: "2f746d702f7365636f6e642e747874" },
  ]
  const selectionWorkspace: WorkspaceSnapshotPayload = {
    ...workspace,
    left: { ...workspace.left, entry_count: entries.length, entries },
  }
  setup = await testRender(() => <App workspace={selectionWorkspace} capabilities={capabilities} onCommand={(command) => { sent.push(command) }} />, { width: 100, height: 20 })
  await setup.renderOnce()

  const secondEntry = setup.renderer.root.findDescendantById("entry-left-1")
  expect(secondEntry).toBeDefined()
  await setup.mockMouse.click(secondEntry!.x, secondEntry!.y)
  expect(sent.at(-1)).toEqual({ action: "select-entry", pane: "left", index: 1, toggle: false })
  await setup.mockMouse.click(secondEntry!.x, secondEntry!.y, MouseButtons.RIGHT)
  expect(sent.at(-1)).toEqual({ action: "select-entry", pane: "left", index: 1, toggle: true })
})

test("does not send mouse selection commands to an older core", async () => {
  const sent: unknown[] = []
  const entries = [
    snapshot.entries[0]!,
    { ...snapshot.entries[0]!, name_display: "second.txt", path_bytes_hex: "2f746d702f7365636f6e642e747874" },
  ]
  const selectionWorkspace: WorkspaceSnapshotPayload = {
    ...workspace,
    left: { ...workspace.left, entry_count: entries.length, entries },
  }
  setup = await testRender(() => <App workspace={selectionWorkspace} onCommand={(command) => { sent.push(command) }} />, { width: 100, height: 20 })
  await setup.renderOnce()

  const secondEntry = setup.renderer.root.findDescendantById("entry-left-1")
  expect(secondEntry).toBeDefined()
  await setup.mockMouse.click(secondEntry!.x, secondEntry!.y)
  await setup.renderOnce()
  expect(sent).toEqual([])
  expect(setup.captureCharFrame()).toContain("Core mouse selection is unavailable")
})

test("toggles the status path style and copies the full displayed path", async () => {
  const copied: string[] = []
  const homeWorkspace: WorkspaceSnapshotPayload = {
    ...workspace,
    left: { ...workspace.left, cwd_display: "/Users/rex/project", cwd_bytes_hex: "2f55736572732f7265782f70726f6a656374" },
  }
  setup = await testRender(() => <App
    workspace={homeWorkspace}
    homeDirectory="/Users/rex"
    onCopyText={(text: string) => { copied.push(text) }}
  />, { width: 100, height: 20 })
  await setup.renderOnce()
  expect(setup.captureCharFrame()).toContain("/Users/rex/project")

  const path = setup.renderer.root.findDescendantById("status-path")
  expect(path).toBeDefined()
  await setup.mockMouse.click(path!.x, path!.y)
  await setup.renderOnce()
  expect(setup.captureCharFrame()).toContain("~/project")
  await setup.mockMouse.click(path!.x, path!.y, MouseButtons.RIGHT)
  expect(copied).toEqual(["~/project"])

  await setup.mockMouse.click(path!.x, path!.y)
  await setup.mockMouse.click(path!.x, path!.y, MouseButtons.RIGHT)
  expect(copied).toEqual(["~/project", "/Users/rex/project"])
})

test("keeps clipboard success and failure feedback visible in a compact terminal", async () => {
  let shouldFail = false
  setup = await testRender(() => <App
    workspace={workspace}
    onCopyText={() => shouldFail ? Promise.reject(new Error("clipboard denied")) : undefined}
  />, { width: 80, height: 20 })
  await setup.renderOnce()

  const path = setup.renderer.root.findDescendantById("status-path")
  expect(path).toBeDefined()
  await setup.mockMouse.click(path!.x, path!.y, MouseButtons.RIGHT)
  await Bun.sleep(0)
  await setup.renderOnce()
  expect(setup.captureCharFrame()).toContain("Copied /tmp")

  shouldFail = true
  await setup.mockMouse.click(path!.x, path!.y, MouseButtons.RIGHT)
  await Bun.sleep(0)
  await setup.renderOnce()
  const failedFrame = setup.captureCharFrame()
  expect(failedFrame).toContain("Copy failed: clipboard denied")
  expect(failedFrame).not.toContain("")
})

test("delete confirmation describes the whole selection instead of only the cursor", async () => {
  const selectedWorkspace: WorkspaceSnapshotPayload = {
    ...workspace,
    left: {
      ...workspace.left,
      selection_count: 1,
      entries: [{ ...workspace.left.entries[0]!, selected: true }],
    },
  }
  setup = await testRender(() => <App workspace={selectedWorkspace} capabilities={capabilities} onCommand={() => true} />, { width: 100, height: 20 })
  await setup.renderOnce()
  setup.mockInput.pressKey("F8")
  await setup.renderOnce()
  expect(setup.captureCharFrame()).toContain("Delete 1 selected items?")
})

test("yanks the cursor entry with yy and puts a copy into the opposite pane with p", async () => {
  const sent: unknown[] = []
  const [props, setProps] = createSignal<AppProps>({ workspace, capabilities, onCommand: (command) => { sent.push(command) } })
  setup = await testRender(() => <App {...props()} />, { width: 100, height: 20 })
  await setup.renderOnce()

  setup.mockInput.pressKey("y")
  setup.mockInput.pressKey("y")
  await setup.renderOnce()
  expect(setup.captureCharFrame()).toContain("1 item(s) yanked")
  expect(sent).toEqual([])

  setProps({ workspace: { ...workspace, active_pane: "right" }, capabilities, onCommand: (command) => { sent.push(command) } })
  await setup.renderOnce()
  setup.mockInput.pressKey("p")
  await setup.renderOnce()
  expect(sent.at(-1)).toEqual({
    action: "copy",
    pane: "left",
    cwd_bytes_hex: snapshot.cwd_bytes_hex,
    snapshot_revision: snapshot.snapshot_revision,
    cwd_device: snapshot.cwd_device,
    cwd_inode: snapshot.cwd_inode,
    cwd_ctime_unix_ns: snapshot.cwd_ctime_unix_ns,
    destination_cwd_bytes_hex: workspace.right.cwd_bytes_hex,
    destination_snapshot_revision: workspace.right.snapshot_revision,
    destination_cwd_device: workspace.right.cwd_device,
    destination_cwd_inode: workspace.right.cwd_inode,
    destination_cwd_ctime_unix_ns: workspace.right.cwd_ctime_unix_ns,
    targets: [{
      path_bytes_hex: snapshot.entries[0]!.path_bytes_hex,
      device: snapshot.entries[0]!.device,
      inode: snapshot.entries[0]!.inode,
      ctime_unix_ns: snapshot.entries[0]!.ctime_unix_ns,
      kind: snapshot.entries[0]!.kind,
    }],
  })
})

test("puts a yanked selection by moving it with P", async () => {
  const sent: unknown[] = []
  const selectedWorkspace: WorkspaceSnapshotPayload = {
    ...workspace,
    left: {
      ...workspace.left,
      selection_count: 2,
      entries: [
        { ...workspace.left.entries[0]!, selected: true },
        { ...workspace.left.entries[0]!, name_display: "b.txt", path_bytes_hex: "beef", device: "12", inode: "22", ctime_unix_ns: "32", selected: true },
        { ...workspace.left.entries[0]!, name_display: "c.txt", path_bytes_hex: "c0ffee" },
      ],
    },
  }
  setup = await testRender(() => <App workspace={selectedWorkspace} capabilities={capabilities} onCommand={(command) => { sent.push(command) }} />, { width: 100, height: 20 })
  await setup.renderOnce()

  setup.mockInput.pressKey("Y")
  await setup.renderOnce()
  expect(setup.captureCharFrame()).toContain("2 item(s) yanked")

  setup.mockInput.pressKey("P")
  await setup.renderOnce()
  expect(sent.at(-1)).toMatchObject({
    action: "move-files",
    pane: "left",
    targets: [
      { path_bytes_hex: snapshot.entries[0]!.path_bytes_hex, inode: "21" },
      { path_bytes_hex: "beef", inode: "22" },
    ],
  })
})

test("reuses the refreshed source identity when putting after a watcher update", async () => {
  const sent: unknown[] = []
  const [props, setProps] = createSignal<AppProps>({ workspace, capabilities, onCommand: (command) => { sent.push(command) } })
  setup = await testRender(() => <App {...props()} />, { width: 100, height: 20 })
  await setup.renderOnce()

  setup.mockInput.pressKey("y")
  setup.mockInput.pressKey("y")
  await setup.renderOnce()

  const refreshed: WorkspaceSnapshotPayload = {
    ...workspace,
    active_pane: "right",
    left: {
      ...workspace.left,
      snapshot_revision: "9",
      entries: [{ ...workspace.left.entries[0]!, inode: "99", ctime_unix_ns: "88" }],
    },
  }
  setProps({ workspace: refreshed, capabilities, onCommand: (command) => { sent.push(command) } })
  await setup.renderOnce()
  setup.mockInput.pressKey("p")
  await setup.renderOnce()
  expect(sent.at(-1)).toMatchObject({
    action: "copy",
    pane: "left",
    snapshot_revision: "9",
    targets: [{ path_bytes_hex: snapshot.entries[0]!.path_bytes_hex, inode: "99", ctime_unix_ns: "88" }],
  })
})

test("refuses to put an empty buffer or a source that left both panes", async () => {
  const sent: unknown[] = []
  const [props, setProps] = createSignal<AppProps>({ workspace, capabilities, onCommand: (command) => { sent.push(command) } })
  setup = await testRender(() => <App {...props()} />, { width: 100, height: 20 })
  await setup.renderOnce()

  setup.mockInput.pressKey("p")
  await setup.renderOnce()
  expect(setup.captureCharFrame()).toContain("Yank buffer is empty")
  expect(sent).toEqual([])

  setup.mockInput.pressKey("y")
  setup.mockInput.pressKey("y")
  await setup.renderOnce()
  setProps({
    workspace: { ...workspace, left: { ...workspace.left, cwd_bytes_hex: "aaaa", entries: [] } },
    capabilities,
    onCommand: (command) => { sent.push(command) },
  })
  await setup.renderOnce()
  setup.mockInput.pressKey("p")
  await setup.renderOnce()
  expect(setup.captureCharFrame()).toContain("Yank source directory is no longer visible")
  expect(sent).toEqual([])
})

test("reports yanked entries that vanished after a refresh", async () => {
  const sent: unknown[] = []
  const [props, setProps] = createSignal<AppProps>({ workspace, capabilities, onCommand: (command) => { sent.push(command) } })
  setup = await testRender(() => <App {...props()} />, { width: 100, height: 20 })
  await setup.renderOnce()

  setup.mockInput.pressKey("y")
  setup.mockInput.pressKey("y")
  await setup.renderOnce()
  setProps({
    workspace: { ...workspace, left: { ...workspace.left, snapshot_revision: "10", entries: [] } },
    capabilities,
    onCommand: (command) => { sent.push(command) },
  })
  await setup.renderOnce()
  setup.mockInput.pressKey("p")
  await setup.renderOnce()
  expect(setup.captureCharFrame()).toContain("Yank source no longer exists: file.txt")
  expect(sent).toEqual([])
})

test("routes dd through the same guarded delete confirmation as F8", async () => {
  setup = await testRender(() => <App workspace={workspace} capabilities={capabilities} onCommand={() => true} />, { width: 100, height: 20 })
  await setup.renderOnce()
  setup.mockInput.pressKey("d")
  await setup.renderOnce()
  expect(setup.captureCharFrame()).not.toContain("Delete file.txt?")
  setup.mockInput.pressKey("d")
  await setup.renderOnce()
  expect(setup.captureCharFrame()).toContain("Delete file.txt?")
})
