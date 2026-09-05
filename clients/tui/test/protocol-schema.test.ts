import { expect, test } from "bun:test"
import { resolve } from "node:path"

import { MAX_PANE_TABS, MAX_SNAPSHOT_ENTRIES, MAX_WORKSPACE_ENTRIES, parseProtocolRecord } from "../src/protocol.js"

function objectValue(value: unknown): Readonly<Record<string, unknown>> {
  if (typeof value !== "object" || value === null || Array.isArray(value)) {
    throw new Error("expected schema object")
  }
  return value as Readonly<Record<string, unknown>>
}

const schemaPath = resolve(import.meta.dir, "../../../protocol/neovifm-core-v0.schema.json")
const schema = objectValue(JSON.parse(await Bun.file(schemaPath).text()))
const definitions = objectValue(schema.$defs)
const base = objectValue(definitions.base)
const baseProperties = objectValue(base.properties)
const snapshot = objectValue(definitions.snapshot)
const snapshotParts = snapshot.allOf
if (!Array.isArray(snapshotParts)) {
  throw new Error("expected snapshot allOf")
}
const snapshotVariant = objectValue(snapshotParts[1])
const snapshotProperties = objectValue(snapshotVariant.properties)
const snapshotPayload = objectValue(snapshotProperties.payload)
const payloadProperties = objectValue(snapshotPayload.properties)
const workspaceSchemaPath = resolve(import.meta.dir, "../../../protocol/neovifm-core-v1.schema.json")
const workspaceSchema = objectValue(JSON.parse(await Bun.file(workspaceSchemaPath).text()))
const workspaceDefinitions = objectValue(workspaceSchema.$defs)
const workspaceSnapshot = objectValue(workspaceDefinitions.workspaceSnapshot)
const workspaceParts = workspaceSnapshot.allOf
if (!Array.isArray(workspaceParts)) throw new Error("expected workspace allOf")
const workspaceVariant = objectValue(workspaceParts[1])
const workspaceProperties = objectValue(workspaceVariant.properties)
const workspacePayload = objectValue(workspaceProperties.payload)
const workspacePayloadProperties = objectValue(workspacePayload.properties)
const sessionSchemaPath = resolve(import.meta.dir, "../../../protocol/neovifm-core-v2.schema.json")
const sessionSchema = objectValue(JSON.parse(await Bun.file(sessionSchemaPath).text()))
const sessionDefinitions = objectValue(sessionSchema.$defs)
const sessionWorkspace = objectValue(sessionDefinitions.workspaceSnapshot)
const sessionParts = sessionWorkspace.allOf
if (!Array.isArray(sessionParts)) throw new Error("expected session workspace allOf")
const sessionVariant = objectValue(sessionParts[1])
const sessionProperties = objectValue(sessionVariant.properties)
const sessionPayload = objectValue(sessionProperties.payload)
const sessionPayloadProperties = objectValue(sessionPayload.properties)
const previewSessionSchemaPath = resolve(import.meta.dir, "../../../protocol/neovifm-core-v3.schema.json")
const previewSessionSchema = objectValue(JSON.parse(await Bun.file(previewSessionSchemaPath).text()))
const previewSessionDefinitions = objectValue(previewSessionSchema.$defs)

test("schema resource bounds match the TypeScript protocol boundary", () => {
  expect(objectValue(baseProperties.sequence).maximum).toBe(Number.MAX_SAFE_INTEGER)
  expect(objectValue(payloadProperties.entry_count).maximum).toBe(MAX_SNAPSHOT_ENTRIES)
  expect(objectValue(payloadProperties.entries).maxItems).toBe(MAX_SNAPSHOT_ENTRIES)
})

test("schema exposes the additive archive resource marker", () => {
  const entry = objectValue(objectValue(definitions.entry))
  expect(objectValue(entry.properties).resource_kind).toEqual({ enum: ["archive"] })
})

test("schema declares the cursor invariant enforced at the runtime boundary", () => {
  expect(snapshot.$comment).toBe(
    "cursor is -1 exactly when entry_count is 0; otherwise 0 <= cursor < entry_count",
  )

  expect(() => parseProtocolRecord({
    protocol: "neovifm-core",
    version: 0,
    type: "snapshot",
    sequence: 1,
    payload: {
      cwd_display: "/tmp",
      cwd_bytes_hex: "2f746d70",
      generated_at_unix_ms: "0",
      cursor: 1,
      entry_count: 1,
      entries: [{
        name_display: "file",
        name_bytes_hex: "66696c65",
        path_display: "/tmp/file",
        path_bytes_hex: "2f746d702f66696c65",
        kind: "file",
        size_bytes: "0",
        mtime_unix_ms: "0",
        selected: false,
        hidden: false,
      }],
    },
  })).toThrow("payload.cursor")
})

test("v1 schema requires both panes and documents the combined entry bound", () => {
  expect(workspacePayload.required).toEqual(["active_pane", "left", "right"])
  expect(objectValue(workspacePayloadProperties.active_pane).enum).toEqual(["left", "right"])
  expect(workspacePayload.$comment).toContain(String(MAX_WORKSPACE_ENTRIES))
})

test("v2 schema requires an acknowledged command sequence and refresh trigger", () => {
  expect(sessionPayload.required).toEqual(["command_sequence", "trigger", "active_pane", "left", "right"])
  expect(objectValue(sessionPayloadProperties.trigger).enum).toEqual(["initial", "command", "watch"])
  const commandError = objectValue(sessionDefinitions.commandError)
  expect(commandError.allOf).toBeDefined()
})

test("v3 schema declares task identity and terminal preview boundaries", () => {
  const taskPayload = objectValue(previewSessionDefinitions.taskPayload)
  expect(taskPayload.required).toEqual(["task_id", "generation", "pane", "kind", "state", "cwd_bytes_hex", "path_bytes_hex"])
  const taskProperties = objectValue(taskPayload.properties)
  expect(objectValue(taskProperties.target_pane).enum).toEqual(["left", "right"])
  expect(objectValue(taskProperties.state).enum).toEqual(["queued", "running", "done", "failed", "cancelled"])
  const preview = objectValue(previewSessionDefinitions.preview)
  expect(preview.allOf).toBeDefined()
})

test("v3 schema exposes core-owned pane toggle and Vifm first/last movement", () => {
  const commandPayload = objectValue(previewSessionDefinitions.commandPayload)
  const serialized = JSON.stringify(commandPayload)
  expect(serialized).toContain('"focus-next"')
  expect(serialized).toContain('"move-to"')
  expect(serialized).toContain('"first"')
  expect(serialized).toContain('"last"')
})

test("v3 schema exposes the core-owned undo command", () => {
  const commandPayload = objectValue(previewSessionDefinitions.commandPayload)
  expect(JSON.stringify(commandPayload)).toContain('"undo"')
})

test("v3 schema exposes the core-owned open resolution and bounded association argv", () => {
  const openPayload = objectValue(previewSessionDefinitions.openPayload)
  expect(openPayload.required).toEqual([
    "command_sequence",
    "intent",
    "source",
    "state",
    "path_bytes_hex",
    "argv",
  ])
  const openProperties = objectValue(openPayload.properties)
  expect(objectValue(openProperties.argv).maxItems).toBe(32)
  const commandPayload = objectValue(previewSessionDefinitions.commandPayload)
  const serialized = JSON.stringify(commandPayload)
  expect(serialized).toContain('"association_argv"')
  expect(serialized).toContain('"path_bytes_hex"')
})

test("v3 schema bounds pane tabs and exposes core-owned tab and mouse commands", () => {
  const paneTabs = objectValue(previewSessionDefinitions.paneTabs)
  expect(paneTabs.minItems).toBe(1)
  expect(paneTabs.maxItems).toBe(MAX_PANE_TABS)
  const paneTab = objectValue(previewSessionDefinitions.paneTab)
  expect(paneTab.required).toEqual(["id", "cwd_display", "active"])
  const tabId = objectValue(previewSessionDefinitions.tabId)
  expect(tabId.pattern).toBe("^[1-9][0-9]*$")

  const commandPayload = objectValue(previewSessionDefinitions.commandPayload)
  const serialized = JSON.stringify(commandPayload)
  expect(serialized).toContain('"select-entry"')
  expect(serialized).toContain('"new-tab"')
  expect(serialized).toContain('"activate-tab"')
  expect(serialized).toContain('"close-tab"')
  expect(serialized).toContain('"tab-cycle"')
  expect(serialized).toContain('"preview"')
  expect(serialized).toContain('"minimum":-999')
  expect(serialized).toContain('"maximum":999')
})

test("v3 file actions require snapshot identities instead of display paths", () => {
  const actionTarget = objectValue(previewSessionDefinitions.actionTarget)
  expect(actionTarget.required).toEqual(["path_bytes_hex", "device", "inode", "ctime_unix_ns", "kind"])
  const actionTargets = objectValue(previewSessionDefinitions.actionTargets)
  expect(actionTargets.maxItems).toBe(64)
  const identity = objectValue(previewSessionDefinitions.sourceIdentity)
  expect(objectValue(identity.properties).cwd_ctime_unix_ns).toBeDefined()
  const actionTask = objectValue(previewSessionDefinitions.actionTaskPayload)
  expect(objectValue(actionTask.properties).undo_available).toEqual({ type: "boolean" })
  expect(objectValue(actionTask.properties).source_path_bytes_hex).toBeDefined()
  expect(objectValue(actionTask.properties).destination_path_bytes_hex).toBeDefined()
  expect(objectValue(actionTask.properties).current_path_bytes_hex).toBeDefined()
  expect(objectValue(actionTask.properties).bytes_completed).toBeDefined()
  expect(objectValue(actionTask.properties).bytes_total).toBeDefined()
  expect(objectValue(actionTask.properties).bytes_known).toEqual({ type: "boolean" })
  expect(objectValue(actionTask.properties).started_at_unix_ms).toBeDefined()
  expect(objectValue(actionTask.properties).finished_at_unix_ms).toBeDefined()
  expect(actionTask.required).toEqual(["task_id", "command_sequence", "pane", "action", "state", "completed_count", "total_count", "partial"])
})

test("v3 schema exposes single-target rename with a bounded new name", () => {
  const commandPayload = objectValue(previewSessionDefinitions.commandPayload)
  const serialized = JSON.stringify(commandPayload)
  expect(serialized).toContain('"rename"')
  // rename keeps its single-target bound next to the action marker.
  expect(serialized).toMatch(/"rename"[\s\S]*?"minItems":1,"maxItems":1/)
  const actionTask = objectValue(previewSessionDefinitions.actionTaskPayload)
  expect(JSON.stringify(objectValue(actionTask.properties).action)).toContain('"rename"')
})

test("v3 preview schema exposes bounded archive listings", () => {
  const taskPayload = objectValue(previewSessionDefinitions.taskPayload)
  const kind = objectValue(objectValue(taskPayload.properties).kind)
  expect(kind.enum).toContain("archive")
  expect(kind.enum).toContain("binary")
  expect(kind.enum).toContain("image")
  expect(kind.enum).toContain("audio")
  expect(kind.enum).toContain("video")
})
