export const PROTOCOL_NAME = "neovifm-core" as const
export const PROTOCOL_VERSION = 0 as const
export const WORKSPACE_PROTOCOL_VERSION = 1 as const
export const SESSION_PROTOCOL_VERSION = 2 as const
export const PREVIEW_SESSION_PROTOCOL_VERSION = 3 as const

const DECIMAL_PATTERN = /^-?[0-9]+$/
const UNSIGNED_DECIMAL_PATTERN = /^[0-9]+$/
const POSITIVE_DECIMAL_PATTERN = /^[1-9][0-9]*$/
const HEX_PATTERN = /^(?:[0-9a-f]{2})*$/
const OCTAL_PATTERN = /^[0-7]+$/
const ATTRIBUTES_PATTERN = /^[0-9a-f]+$/

export const MAX_PROTOCOL_RECORD_BYTES = 4 * 1024 * 1024
export const MAX_PROTOCOL_TOTAL_BYTES = 8 * 1024 * 1024
export const MAX_PROTOCOL_RECORDS = 2
export const MAX_SNAPSHOT_ENTRIES = 4096
export const MAX_WORKSPACE_ENTRIES = 4096
export const MAX_PANE_TABS = 8

const MAX_DISPLAY_TEXT_BYTES = 16 * 1024
const MAX_HEX_TEXT_BYTES = 32 * 1024
const MAX_DECIMAL_TEXT_BYTES = 32
const MAX_UINT64_TEXT_BYTES = 20
const MAX_CAPABILITIES = 64
const MAX_CAPABILITY_BYTES = 256
const MAX_ERROR_CODE_BYTES = 128
const MAX_PREVIEW_TEXT_BYTES = 64 * 1024
const MAX_OWNER_GROUP_BYTES = 256
const MAX_OPEN_ARGS = 32
const MAX_OPEN_ARG_BYTES = 4 * 1024
const UTF8_ENCODER = new TextEncoder()

export type EntryKind =
  | "directory"
  | "file"
  | "symlink"
  | "executable"
  | "fifo"
  | "socket"
  | "char-device"
  | "block-device"
  | "unknown"

export type EntryResourceKind = "archive"

export interface StatError {
  readonly code: number
  readonly message: string
}

export interface SnapshotEntry {
  readonly name_display: string
  readonly name_bytes_hex: string
  readonly path_display: string
  readonly path_bytes_hex: string
  readonly kind: EntryKind
  readonly resource_kind?: EntryResourceKind
  readonly size_bytes: string
  readonly mtime_unix_ms: string
  readonly device?: string
  readonly inode?: string
  readonly ctime_unix_ns?: string
  readonly mode_octal?: string
  readonly attributes_hex?: string
  readonly owner_display?: string
  readonly group_display?: string
  readonly selected: boolean
  readonly hidden: boolean
  readonly stat_error?: StatError
}

export interface HelloPayload {
  readonly implementation: string
  readonly capabilities: readonly string[]
}

export interface SnapshotPayload {
  readonly cwd_display: string
  readonly cwd_bytes_hex: string
  readonly generated_at_unix_ms: string
  readonly snapshot_revision: string
  readonly cwd_device?: string
  readonly cwd_inode?: string
  readonly cwd_ctime_unix_ns?: string
  readonly cursor: number
  readonly entry_count: number
  readonly selection_count: number
  readonly filtered_count: number
  readonly sort_key: PaneSortKey
  readonly sort_descending: boolean
  readonly filter_active: boolean
  readonly entries: readonly SnapshotEntry[]
}

export interface ErrorPayload {
  readonly code: string
  readonly message: string
  readonly retryable: boolean
  readonly path_display?: string
  readonly path_bytes_hex?: string
  readonly os_error?: number
}

export type PaneId = "left" | "right"
export type PaneSortKey = "name" | "extension" | "size" | "ctime" | "mtime" | "mode" | "type" | "other"
export type SessionSnapshotTrigger = "initial" | "command" | "watch" | "action" | "resource"

export interface PaneTabPayload {
  readonly id: string
  readonly cwd_display: string
  readonly active: boolean
  readonly resource_kind?: "archive" | "ssh"
}

export interface WorkspaceSnapshotPayload {
  readonly active_pane: PaneId
  readonly left: SnapshotPayload
  readonly right: SnapshotPayload
  readonly left_tabs?: readonly PaneTabPayload[]
  readonly right_tabs?: readonly PaneTabPayload[]
}

export interface SessionWorkspaceSnapshotPayload extends WorkspaceSnapshotPayload {
  readonly command_sequence: number
  readonly trigger: SessionSnapshotTrigger
}

export interface CommandErrorPayload extends ErrorPayload {
  readonly command_sequence: number
}

export type PreviewKind = "text" | "markdown" | "pdf" | "directory" | "archive" | "binary" | "image" | "audio" | "video"
export type PreviewTaskState = "queued" | "running" | "done" | "failed" | "cancelled"

export interface PreviewTaskPayload {
  readonly task_id: string
  readonly generation: string
  readonly pane: PaneId
  readonly target_pane?: PaneId
  readonly kind: PreviewKind
  readonly state: PreviewTaskState
  readonly cwd_bytes_hex: string
  readonly path_bytes_hex: string
  readonly error_code?: string
  readonly os_error?: number
}

export interface PreviewPayload extends PreviewTaskPayload {
  readonly content?: string
  readonly truncated: boolean
}

export type ActionTaskAction = "copy" | "move" | "mkdir" | "delete" | "rename"

export interface ActionTaskPayload {
  readonly task_id: string
  readonly command_sequence: number
  readonly pane: PaneId
  readonly action: ActionTaskAction
  readonly state: PreviewTaskState
  readonly completed_count: number
  readonly total_count: number
  readonly failed_index?: number
  readonly partial: boolean
  readonly retryable: boolean
  readonly undo_available?: boolean
  readonly source_path_bytes_hex?: string
  readonly destination_path_bytes_hex?: string
  readonly current_path_bytes_hex?: string
  readonly bytes_known?: boolean
  readonly bytes_completed?: string
  readonly bytes_total?: string
  readonly started_at_unix_ms?: string
  readonly finished_at_unix_ms?: string
  readonly error_code?: string
  readonly os_error?: number
}

export type ResourceTaskKind = "mount-archive" | "mount-ssh" | "unmount"

export interface ResourceTaskPayload {
  readonly task_id: string
  readonly command_sequence: number
  readonly pane: PaneId
  readonly tab_id: string
  readonly resource: ResourceTaskKind
  readonly state: PreviewTaskState
  readonly source_path?: string
  readonly mount_point?: string
  readonly unmount_path?: string
  readonly error_code?: string
  readonly os_error?: number
}

export type OpenIntent = "open" | "edit" | "preview"
export type OpenSource = "association" | "platform"
export interface OpenPayload {
  readonly command_sequence: number
  readonly intent: OpenIntent
  readonly source: OpenSource
  readonly state: "resolved"
  readonly path_bytes_hex: string
  readonly argv: readonly string[]
}

interface Envelope<Type extends string, Payload, Version extends number> {
  readonly protocol: typeof PROTOCOL_NAME
  readonly version: Version
  readonly type: Type
  readonly sequence: number
  readonly payload: Payload
}

export type HelloRecord = Envelope<"hello", HelloPayload, typeof PROTOCOL_VERSION>
export type SnapshotRecord = Envelope<"snapshot", SnapshotPayload, typeof PROTOCOL_VERSION>
export type V0ErrorRecord = Envelope<"error", ErrorPayload, typeof PROTOCOL_VERSION>
export type WorkspaceHelloRecord = Envelope<"hello", HelloPayload, typeof WORKSPACE_PROTOCOL_VERSION>
export type WorkspaceSnapshotRecord = Envelope<
  "workspace-snapshot",
  WorkspaceSnapshotPayload,
  typeof WORKSPACE_PROTOCOL_VERSION
>
export type V1ErrorRecord = Envelope<"error", ErrorPayload, typeof WORKSPACE_PROTOCOL_VERSION>
export type SessionHelloRecord = Envelope<"hello", HelloPayload, typeof SESSION_PROTOCOL_VERSION>
export type SessionWorkspaceSnapshotRecord = Envelope<
  "workspace-snapshot",
  SessionWorkspaceSnapshotPayload,
  typeof SESSION_PROTOCOL_VERSION
>
export type CommandErrorRecord = Envelope<"command-error", CommandErrorPayload, typeof SESSION_PROTOCOL_VERSION>
export type SessionErrorRecord = Envelope<"error", ErrorPayload, typeof SESSION_PROTOCOL_VERSION>
export type PreviewSessionHelloRecord = Envelope<"hello", HelloPayload, typeof PREVIEW_SESSION_PROTOCOL_VERSION>
export type PreviewSessionWorkspaceSnapshotRecord = Envelope<
  "workspace-snapshot",
  SessionWorkspaceSnapshotPayload,
  typeof PREVIEW_SESSION_PROTOCOL_VERSION
>
export type PreviewTaskRecord = Envelope<"task", PreviewTaskPayload, typeof PREVIEW_SESSION_PROTOCOL_VERSION>
export type PreviewRecord = Envelope<"preview", PreviewPayload, typeof PREVIEW_SESSION_PROTOCOL_VERSION>
export type ActionTaskRecord = Envelope<"action-task", ActionTaskPayload, typeof PREVIEW_SESSION_PROTOCOL_VERSION>
export type ResourceTaskRecord = Envelope<"resource-task", ResourceTaskPayload, typeof PREVIEW_SESSION_PROTOCOL_VERSION>
export type OpenRecord = Envelope<"open", OpenPayload, typeof PREVIEW_SESSION_PROTOCOL_VERSION>
export type PreviewSessionCommandErrorRecord = Envelope<"command-error", CommandErrorPayload, typeof PREVIEW_SESSION_PROTOCOL_VERSION>
export type PreviewSessionErrorRecord = Envelope<"error", ErrorPayload, typeof PREVIEW_SESSION_PROTOCOL_VERSION>
export type ErrorRecord = V0ErrorRecord | V1ErrorRecord | SessionErrorRecord | PreviewSessionErrorRecord
export type ProtocolRecord =
  | HelloRecord
  | SnapshotRecord
  | V0ErrorRecord
  | WorkspaceHelloRecord
  | WorkspaceSnapshotRecord
  | V1ErrorRecord
  | SessionHelloRecord
  | SessionWorkspaceSnapshotRecord
  | CommandErrorRecord
  | SessionErrorRecord
  | PreviewSessionHelloRecord
  | PreviewSessionWorkspaceSnapshotRecord
  | PreviewTaskRecord
  | PreviewRecord
  | ActionTaskRecord
  | ResourceTaskRecord
  | OpenRecord
  | PreviewSessionCommandErrorRecord
  | PreviewSessionErrorRecord

export interface JsonlLimits {
  readonly maximumRecordBytes?: number
  readonly maximumTotalBytes?: number
  readonly maximumRecords?: number
}

type UnknownObject = Readonly<Record<string, unknown>>

export class ProtocolValidationError extends Error {
  override readonly name = "ProtocolValidationError"
}

function invalid(path: string, expectation: string): never {
  throw new ProtocolValidationError(`${path} ${expectation}`)
}

function frozen<Value extends object>(value: Value): Value {
  return Object.freeze(value) as Value
}

function textByteLength(value: string): number {
  return UTF8_ENCODER.encode(value).byteLength
}

function objectValue(value: unknown, path: string): UnknownObject {
  if (typeof value !== "object" || value === null || Array.isArray(value)) {
    return invalid(path, "must be an object")
  }
  return value as UnknownObject
}

function stringValue(value: unknown, path: string, allowEmpty = true): string {
  if (typeof value !== "string" || (!allowEmpty && value.length === 0)) {
    return invalid(path, allowEmpty ? "must be a string" : "must be a non-empty string")
  }
  return value
}

function boundedString(
  value: unknown,
  path: string,
  maximumBytes: number,
  allowEmpty = true,
): string {
  const text = stringValue(value, path, allowEmpty)
  if (textByteLength(text) > maximumBytes) {
    return invalid(path, `must not exceed ${maximumBytes} UTF-8 bytes`)
  }
  return text
}

function patternString(
  value: unknown,
  path: string,
  pattern: RegExp,
  maximumBytes = MAX_HEX_TEXT_BYTES,
): string {
  const text = boundedString(value, path, maximumBytes)
  if (!pattern.test(text)) {
    return invalid(path, "has an invalid format")
  }
  return text
}

function integerValue(value: unknown, path: string, minimum?: number): number {
  if (!Number.isSafeInteger(value) || (minimum !== undefined && (value as number) < minimum)) {
    return invalid(path, `must be a safe integer${minimum === undefined ? "" : ` >= ${minimum}`}`)
  }
  return value as number
}

function booleanValue(value: unknown, path: string): boolean {
  if (typeof value !== "boolean") {
    return invalid(path, "must be a boolean")
  }
  return value
}

function isUnsafeDisplayCodePoint(codePoint: number): boolean {
  return (
    codePoint <= 0x1f
    || codePoint === 0x7f
    || (codePoint >= 0x80 && codePoint <= 0x9f)
    || (codePoint >= 0xd800 && codePoint <= 0xdfff)
    || codePoint === 0x061c
    || codePoint === 0x200e
    || codePoint === 0x200f
    || (codePoint >= 0x2028 && codePoint <= 0x202e)
    || (codePoint >= 0x2066 && codePoint <= 0x2069)
  )
}

function sanitizeText(text: string, preserveWhitespace: boolean): string {
  let display = ""
  for (const character of text) {
    const codePoint = character.codePointAt(0)
    const safeWhitespace = preserveWhitespace && codePoint !== undefined &&
      (codePoint === 0x09 || codePoint === 0x0a || codePoint === 0x0d)
    display += codePoint !== undefined && isUnsafeDisplayCodePoint(codePoint) && !safeWhitespace ? "�" : character
  }
  return display
}

export function sanitizeDisplayText(text: string): string {
  return sanitizeText(text, false)
}

export function sanitizePreviewText(text: string): string {
  return sanitizeText(text, true)
}

function displayString(value: unknown, path: string, allowEmpty = true, maximumBytes = MAX_DISPLAY_TEXT_BYTES): string {
  const text = boundedString(value, path, maximumBytes, allowEmpty)
  const sanitized = sanitizeDisplayText(text)
  if (textByteLength(sanitized) > maximumBytes) {
    return invalid(path, `must not exceed ${maximumBytes} UTF-8 bytes after sanitization`)
  }
  return sanitized
}

function optionalDisplayString(object: UnknownObject, key: string, path: string, maximumBytes = MAX_DISPLAY_TEXT_BYTES): string | undefined {
  const value = object[key]
  return value === undefined ? undefined : displayString(value, `${path}.${key}`, true, maximumBytes)
}

function optionalPatternString(
  object: UnknownObject,
  key: string,
  path: string,
  pattern: RegExp,
  maximumBytes = MAX_HEX_TEXT_BYTES,
): string | undefined {
  const value = object[key]
  return value === undefined ? undefined : patternString(value, `${path}.${key}`, pattern, maximumBytes)
}

function parseStringArray(value: unknown, path: string): readonly string[] {
  if (!Array.isArray(value)) {
    return invalid(path, "must be an array")
  }
  if (value.length > MAX_CAPABILITIES) {
    return invalid(path, `must not contain more than ${MAX_CAPABILITIES} values`)
  }
  const result = value.map((item, index) =>
    boundedString(item, `${path}[${index}]`, MAX_CAPABILITY_BYTES, false),
  )
  if (new Set(result).size !== result.length) {
    return invalid(path, "must contain unique values")
  }
  return frozen(result)
}

const ENTRY_KINDS: ReadonlySet<string> = new Set<EntryKind>([
  "directory",
  "file",
  "symlink",
  "executable",
  "fifo",
  "socket",
  "char-device",
  "block-device",
  "unknown",
])

function parseEntryKind(value: unknown, path: string): EntryKind {
  const kind = stringValue(value, path)
  if (!ENTRY_KINDS.has(kind)) {
    return invalid(path, "is not a supported entry kind")
  }
  return kind as EntryKind
}

function parseEntryResourceKind(value: unknown, path: string): EntryResourceKind | undefined {
  if (value === undefined) return undefined
  const resource = stringValue(value, path)
  if (resource !== "archive") return invalid(path, "is not a supported resource kind")
  return resource
}

function parseStatError(value: unknown, path: string): StatError | undefined {
  if (value === undefined) {
    return undefined
  }
  const object = objectValue(value, path)
  return frozen({
    code: integerValue(object.code, `${path}.code`),
    message: displayString(object.message, `${path}.message`),
  })
}

function parseEntry(value: unknown, path: string): SnapshotEntry {
  const object = objectValue(value, path)
  const device = optionalPatternString(object, "device", path, UNSIGNED_DECIMAL_PATTERN, MAX_DECIMAL_TEXT_BYTES)
  const inode = optionalPatternString(object, "inode", path, UNSIGNED_DECIMAL_PATTERN, MAX_DECIMAL_TEXT_BYTES)
  const ctime = optionalPatternString(object, "ctime_unix_ns", path, UNSIGNED_DECIMAL_PATTERN, MAX_DECIMAL_TEXT_BYTES)
  const mode = optionalPatternString(object, "mode_octal", path, OCTAL_PATTERN, MAX_DECIMAL_TEXT_BYTES)
  const attributes = optionalPatternString(object, "attributes_hex", path, ATTRIBUTES_PATTERN)
  const owner = optionalDisplayString(object, "owner_display", path, MAX_OWNER_GROUP_BYTES)
  const group = optionalDisplayString(object, "group_display", path, MAX_OWNER_GROUP_BYTES)
  const resourceKind = parseEntryResourceKind(object.resource_kind, `${path}.resource_kind`)
  const statError = parseStatError(object.stat_error, `${path}.stat_error`)

  return frozen({
    name_display: displayString(object.name_display, `${path}.name_display`),
    name_bytes_hex: patternString(object.name_bytes_hex, `${path}.name_bytes_hex`, HEX_PATTERN),
    path_display: displayString(object.path_display, `${path}.path_display`),
    path_bytes_hex: patternString(object.path_bytes_hex, `${path}.path_bytes_hex`, HEX_PATTERN),
    kind: parseEntryKind(object.kind, `${path}.kind`),
    ...(resourceKind === undefined ? {} : { resource_kind: resourceKind }),
    size_bytes: patternString(
      object.size_bytes,
      `${path}.size_bytes`,
      UNSIGNED_DECIMAL_PATTERN,
      MAX_DECIMAL_TEXT_BYTES,
    ),
    mtime_unix_ms: patternString(
      object.mtime_unix_ms,
      `${path}.mtime_unix_ms`,
      DECIMAL_PATTERN,
      MAX_DECIMAL_TEXT_BYTES,
    ),
    ...(device === undefined ? {} : { device }),
    ...(inode === undefined ? {} : { inode }),
    ...(ctime === undefined ? {} : { ctime_unix_ns: ctime }),
    ...(mode === undefined ? {} : { mode_octal: mode }),
    ...(attributes === undefined ? {} : { attributes_hex: attributes }),
    ...(owner === undefined ? {} : { owner_display: owner }),
    ...(group === undefined ? {} : { group_display: group }),
    selected: booleanValue(object.selected, `${path}.selected`),
    hidden: booleanValue(object.hidden, `${path}.hidden`),
    ...(statError === undefined ? {} : { stat_error: statError }),
  })
}

function parseHelloPayload(value: unknown): HelloPayload {
  const payload = objectValue(value, "payload")
  return frozen({
    implementation: displayString(payload.implementation, "payload.implementation", false),
    capabilities: parseStringArray(payload.capabilities, "payload.capabilities"),
  })
}

function parseSnapshotPayload(value: unknown, path = "payload"): SnapshotPayload {
  const payload = objectValue(value, path)
  if (!Array.isArray(payload.entries)) {
    return invalid(`${path}.entries`, "must be an array")
  }
  if (payload.entries.length > MAX_SNAPSHOT_ENTRIES) {
    return invalid(`${path}.entries`, `must not contain more than ${MAX_SNAPSHOT_ENTRIES} entries`)
  }

  const entryCount = integerValue(payload.entry_count, `${path}.entry_count`, 0)
  if (entryCount > MAX_SNAPSHOT_ENTRIES) {
    return invalid(`${path}.entry_count`, `must not exceed ${MAX_SNAPSHOT_ENTRIES}`)
  }
  if (entryCount !== payload.entries.length) {
    return invalid(`${path}.entry_count`, "must match payload.entries.length")
  }
  const entries = frozen(payload.entries.map((entry, index) => parseEntry(entry, `${path}.entries[${index}]`)))

  const cursor = integerValue(payload.cursor, `${path}.cursor`, -1)
  if (
    (entryCount === 0 && cursor !== -1)
    || (entryCount !== 0 && (cursor < 0 || cursor >= entryCount))
  ) {
    return invalid(`${path}.cursor`, "must identify an entry or be -1 for an empty snapshot")
  }
  const selectedEntries = entries.filter((entry) => entry.selected).length
  const selectionCount = payload.selection_count === undefined
    ? selectedEntries
    : integerValue(payload.selection_count, `${path}.selection_count`, 0)
  if (selectionCount > entryCount || selectionCount !== selectedEntries) {
    return invalid(`${path}.selection_count`, "must match selected entries")
  }
  const filteredCount = payload.filtered_count === undefined
    ? 0
    : integerValue(payload.filtered_count, `${path}.filtered_count`, 0)
  if (filteredCount > MAX_SNAPSHOT_ENTRIES) return invalid(`${path}.filtered_count`, `must not exceed ${MAX_SNAPSHOT_ENTRIES}`)
  const sortKey = payload.sort_key === undefined ? "name" : parsePaneSortKey(payload.sort_key, `${path}.sort_key`)
  const sortDescending = payload.sort_descending === undefined ? false : booleanValue(payload.sort_descending, `${path}.sort_descending`)
  const filterActive = payload.filter_active === undefined ? false : booleanValue(payload.filter_active, `${path}.filter_active`)

  return frozen({
    cwd_display: displayString(payload.cwd_display, `${path}.cwd_display`),
    cwd_bytes_hex: patternString(payload.cwd_bytes_hex, `${path}.cwd_bytes_hex`, HEX_PATTERN),
    generated_at_unix_ms: patternString(
      payload.generated_at_unix_ms,
      `${path}.generated_at_unix_ms`,
      DECIMAL_PATTERN,
      MAX_DECIMAL_TEXT_BYTES,
    ),
    snapshot_revision: payload.snapshot_revision === undefined
      ? "0"
      : patternString(payload.snapshot_revision, `${path}.snapshot_revision`, UNSIGNED_DECIMAL_PATTERN, MAX_DECIMAL_TEXT_BYTES),
    ...(payload.cwd_device === undefined ? {} : {
      cwd_device: patternString(payload.cwd_device, `${path}.cwd_device`, UNSIGNED_DECIMAL_PATTERN, MAX_DECIMAL_TEXT_BYTES),
    }),
    ...(payload.cwd_inode === undefined ? {} : {
      cwd_inode: patternString(payload.cwd_inode, `${path}.cwd_inode`, UNSIGNED_DECIMAL_PATTERN, MAX_DECIMAL_TEXT_BYTES),
    }),
    ...(payload.cwd_ctime_unix_ns === undefined ? {} : {
      cwd_ctime_unix_ns: patternString(payload.cwd_ctime_unix_ns, `${path}.cwd_ctime_unix_ns`, UNSIGNED_DECIMAL_PATTERN, MAX_DECIMAL_TEXT_BYTES),
    }),
    cursor,
    entry_count: entryCount,
    selection_count: selectionCount,
    filtered_count: filteredCount,
    sort_key: sortKey,
    sort_descending: sortDescending,
    filter_active: filterActive,
    entries,
  })
}

function parsePaneId(value: unknown, path: string): PaneId {
  const pane = stringValue(value, path)
  if (pane !== "left" && pane !== "right") {
    return invalid(path, "must be left or right")
  }
  return pane
}

function parsePaneSortKey(value: unknown, path: string): PaneSortKey {
  const key = stringValue(value, path)
  if (key !== "name" && key !== "extension" && key !== "size" && key !== "ctime" && key !== "mtime" && key !== "mode" && key !== "type" && key !== "other") {
    return invalid(path, "is not a supported sort key")
  }
  return key
}

function parseSessionSnapshotTrigger(value: unknown, path: string): SessionSnapshotTrigger {
  const trigger = stringValue(value, path)
  if (trigger !== "initial" && trigger !== "command" && trigger !== "watch" && trigger !== "action" && trigger !== "resource") {
    return invalid(path, "must be initial, command, watch, action, or resource")
  }
  return trigger
}

function parsePaneTabs(
  value: unknown,
  path: string,
  snapshot: SnapshotPayload,
): readonly PaneTabPayload[] {
  if (value === undefined) {
    return frozen([frozen({ id: "0", cwd_display: snapshot.cwd_display, active: true })])
  }
  if (!Array.isArray(value) || value.length === 0 || value.length > MAX_PANE_TABS) {
    return invalid(path, `must contain between 1 and ${MAX_PANE_TABS} tabs`)
  }
  const ids = new Set<string>()
  const tabs = value.map((entry, index) => {
    const object = objectValue(entry, `${path}[${index}]`)
    const id = patternString(object.id, `${path}[${index}].id`, POSITIVE_DECIMAL_PATTERN, MAX_UINT64_TEXT_BYTES)
    if (ids.has(id)) return invalid(path, "must contain unique tab ids")
    ids.add(id)
    return frozen({
      id,
      cwd_display: displayString(object.cwd_display, `${path}[${index}].cwd_display`),
      active: booleanValue(object.active, `${path}[${index}].active`),
      ...(object.resource_kind === undefined ? {} : {
        resource_kind: (() => {
          const resource = stringValue(object.resource_kind, `${path}[${index}].resource_kind`)
          if (resource === "archive") return "archive" as const
          if (resource === "ssh") return "ssh" as const
          return invalid(`${path}[${index}].resource_kind`, "must be archive or ssh")
        })(),
      }),
    })
  })
  const activeTabs = tabs.filter((tab) => tab.active)
  if (activeTabs.length !== 1) return invalid(path, "must contain exactly one active tab")
  if (activeTabs[0]!.cwd_display !== snapshot.cwd_display) {
    return invalid(path, "active tab must match the pane cwd_display")
  }
  return frozen(tabs)
}

function parseWorkspaceSnapshotPayload(value: unknown): WorkspaceSnapshotPayload {
  const payload = objectValue(value, "payload")
  const left = parseSnapshotPayload(payload.left, "payload.left")
  const right = parseSnapshotPayload(payload.right, "payload.right")
  if (left.entry_count + right.entry_count > MAX_WORKSPACE_ENTRIES) {
    return invalid("payload", `must not contain more than ${MAX_WORKSPACE_ENTRIES} combined entries`)
  }
  return frozen({
    active_pane: parsePaneId(payload.active_pane, "payload.active_pane"),
    left,
    right,
    left_tabs: parsePaneTabs(payload.left_tabs, "payload.left_tabs", left),
    right_tabs: parsePaneTabs(payload.right_tabs, "payload.right_tabs", right),
  })
}

function parseSessionWorkspaceSnapshotPayload(value: unknown): SessionWorkspaceSnapshotPayload {
  const payload = objectValue(value, "payload")
  const workspace = parseWorkspaceSnapshotPayload(payload)
  return frozen({
    ...workspace,
    command_sequence: integerValue(payload.command_sequence, "payload.command_sequence", 0),
    trigger: parseSessionSnapshotTrigger(payload.trigger, "payload.trigger"),
  })
}

function parseErrorPayload(value: unknown): ErrorPayload {
  const payload = objectValue(value, "payload")
  const pathDisplay = optionalDisplayString(payload, "path_display", "payload")
  const pathBytes = optionalPatternString(payload, "path_bytes_hex", "payload", HEX_PATTERN)
  const osError = payload.os_error === undefined
    ? undefined
    : integerValue(payload.os_error, "payload.os_error")

  return frozen({
    code: boundedString(payload.code, "payload.code", MAX_ERROR_CODE_BYTES, false),
    message: displayString(payload.message, "payload.message"),
    retryable: booleanValue(payload.retryable, "payload.retryable"),
    ...(pathDisplay === undefined ? {} : { path_display: pathDisplay }),
    ...(pathBytes === undefined ? {} : { path_bytes_hex: pathBytes }),
    ...(osError === undefined ? {} : { os_error: osError }),
  })
}

function parseCommandErrorPayload(value: unknown): CommandErrorPayload {
  const payload = objectValue(value, "payload")
  const error = parseErrorPayload(payload)
  return frozen({
    ...error,
    command_sequence: integerValue(payload.command_sequence, "payload.command_sequence", 1),
  })
}

function parsePreviewKind(value: unknown, path: string): PreviewKind {
  const kind = stringValue(value, path)
  if (kind !== "text" && kind !== "markdown" && kind !== "pdf" && kind !== "directory" && kind !== "archive" && kind !== "binary" && kind !== "image" && kind !== "audio" && kind !== "video") return invalid(path, "must be text, markdown, pdf, directory, archive, binary, image, audio, or video")
  return kind
}

function parsePreviewTaskState(value: unknown, path: string): PreviewTaskState {
  const state = stringValue(value, path)
  if (state !== "queued" && state !== "running" && state !== "done" && state !== "failed" && state !== "cancelled") {
    return invalid(path, "is not a supported task state")
  }
  return state
}

function parsePreviewTaskPayload(value: unknown, path = "payload"): PreviewTaskPayload {
  const payload = objectValue(value, path)
  const pane = parsePaneId(payload.pane, `${path}.pane`)
  const targetPane = payload.target_pane === undefined
    ? pane
    : parsePaneId(payload.target_pane, `${path}.target_pane`)
  const errorCode = payload.error_code === undefined
    ? undefined
    : boundedString(payload.error_code, `${path}.error_code`, MAX_ERROR_CODE_BYTES, false)
  const osError = payload.os_error === undefined ? undefined : integerValue(payload.os_error, `${path}.os_error`)
  return frozen({
    task_id: patternString(payload.task_id, `${path}.task_id`, UNSIGNED_DECIMAL_PATTERN, MAX_DECIMAL_TEXT_BYTES),
    generation: patternString(payload.generation, `${path}.generation`, UNSIGNED_DECIMAL_PATTERN, MAX_DECIMAL_TEXT_BYTES),
    pane,
    target_pane: targetPane,
    kind: parsePreviewKind(payload.kind, `${path}.kind`),
    state: parsePreviewTaskState(payload.state, `${path}.state`),
    cwd_bytes_hex: patternString(payload.cwd_bytes_hex, `${path}.cwd_bytes_hex`, HEX_PATTERN),
    path_bytes_hex: patternString(payload.path_bytes_hex, `${path}.path_bytes_hex`, HEX_PATTERN),
    ...(errorCode === undefined ? {} : { error_code: errorCode }),
    ...(osError === undefined ? {} : { os_error: osError }),
  })
}

function parsePreviewPayload(value: unknown): PreviewPayload {
  const payload = objectValue(value, "payload")
  const task = parsePreviewTaskPayload(payload)
  if (task.state !== "done" && task.state !== "failed" && task.state !== "cancelled") {
    return invalid("payload.state", "must be a terminal task state for preview")
  }
  const content = payload.content === undefined
    ? undefined
    : boundedString(payload.content, "payload.content", MAX_PREVIEW_TEXT_BYTES)
  if (task.state === "done" && content === undefined) return invalid("payload.content", "must be present for completed preview")
  return frozen({
    ...task,
    ...(content === undefined ? {} : { content: sanitizePreviewText(content) }),
    truncated: booleanValue(payload.truncated, "payload.truncated"),
  })
}

function parseActionTaskPayload(value: unknown): ActionTaskPayload {
  const payload = objectValue(value, "payload")
  const action = stringValue(payload.action, "payload.action")
  if (action !== "copy" && action !== "move" && action !== "mkdir" && action !== "delete" && action !== "rename") {
    return invalid("payload.action", "is not a supported file action")
  }
  const state = parsePreviewTaskState(payload.state, "payload.state")
  const completed = integerValue(payload.completed_count, "payload.completed_count", 0)
  const total = integerValue(payload.total_count, "payload.total_count", 1)
  if (total > 64 || completed > total) return invalid("payload", "has invalid action progress counts")
  const failedIndex = payload.failed_index === undefined
    ? undefined
    : integerValue(payload.failed_index, "payload.failed_index", 0)
  if (failedIndex !== undefined && failedIndex >= total) return invalid("payload.failed_index", "must identify an action target")
  const partial = booleanValue(payload.partial, "payload.partial")
  const retryable = payload.retryable === undefined
    ? false
    : booleanValue(payload.retryable, "payload.retryable")
  const undoAvailable = payload.undo_available === undefined
    ? false
    : booleanValue(payload.undo_available, "payload.undo_available")
  const sourcePath = optionalPatternString(payload, "source_path_bytes_hex", "payload", HEX_PATTERN)
  const destinationPath = optionalPatternString(payload, "destination_path_bytes_hex", "payload", HEX_PATTERN)
  const currentPath = optionalPatternString(payload, "current_path_bytes_hex", "payload", HEX_PATTERN)
  const bytesKnown = payload.bytes_known === undefined
    ? false
    : booleanValue(payload.bytes_known, "payload.bytes_known")
  const bytesCompleted = optionalPatternString(payload, "bytes_completed", "payload", UNSIGNED_DECIMAL_PATTERN, MAX_DECIMAL_TEXT_BYTES)
  const bytesTotal = optionalPatternString(payload, "bytes_total", "payload", UNSIGNED_DECIMAL_PATTERN, MAX_DECIMAL_TEXT_BYTES)
  const startedAt = optionalPatternString(payload, "started_at_unix_ms", "payload", UNSIGNED_DECIMAL_PATTERN, MAX_DECIMAL_TEXT_BYTES)
  const finishedAt = optionalPatternString(payload, "finished_at_unix_ms", "payload", UNSIGNED_DECIMAL_PATTERN, MAX_DECIMAL_TEXT_BYTES)
  if (bytesKnown && (bytesCompleted === undefined || bytesTotal === undefined)) {
    return invalid("payload.bytes_known", "requires bytes_completed and bytes_total")
  }
  if (!bytesKnown && (bytesCompleted !== undefined || bytesTotal !== undefined)) {
    return invalid("payload.bytes_known", "must be true when byte progress is present")
  }
  if (bytesKnown && BigInt(bytesCompleted!) > BigInt(bytesTotal!)) {
    return invalid("payload.bytes_completed", "must not exceed bytes_total")
  }
  if (startedAt !== undefined && finishedAt !== undefined && BigInt(finishedAt) < BigInt(startedAt)) {
    return invalid("payload.finished_at_unix_ms", "must not be earlier than started_at_unix_ms")
  }
  if (state === "done" && partial) return invalid("payload.partial", "must be false for a completed action")
  if (state === "done" && completed !== total) return invalid("payload.completed_count", "must equal total_count for done")
  if (state === "queued" && completed !== 0) return invalid("payload.completed_count", "must be zero before running")
  if ((state === "failed" || state === "cancelled") && failedIndex === undefined) return invalid("payload.failed_index", "is required for a terminal incomplete action")
  if (retryable && state !== "failed" && state !== "cancelled") return invalid("payload.retryable", "requires a failed or cancelled action")
  const errorCode = payload.error_code === undefined
    ? undefined
    : boundedString(payload.error_code, "payload.error_code", MAX_ERROR_CODE_BYTES, false)
  const osError = payload.os_error === undefined ? undefined : integerValue(payload.os_error, "payload.os_error")
  return frozen({
    task_id: patternString(payload.task_id, "payload.task_id", UNSIGNED_DECIMAL_PATTERN, MAX_DECIMAL_TEXT_BYTES),
    command_sequence: integerValue(payload.command_sequence, "payload.command_sequence", 1),
    pane: parsePaneId(payload.pane, "payload.pane"),
    action,
    state,
    completed_count: completed,
    total_count: total,
    ...(failedIndex === undefined ? {} : { failed_index: failedIndex }),
    partial,
    retryable,
    undo_available: undoAvailable,
    ...(sourcePath === undefined ? {} : { source_path_bytes_hex: sourcePath }),
    ...(destinationPath === undefined ? {} : { destination_path_bytes_hex: destinationPath }),
    ...(currentPath === undefined ? {} : { current_path_bytes_hex: currentPath }),
    bytes_known: bytesKnown,
    ...(bytesCompleted === undefined ? {} : { bytes_completed: bytesCompleted }),
    ...(bytesTotal === undefined ? {} : { bytes_total: bytesTotal }),
    ...(startedAt === undefined ? {} : { started_at_unix_ms: startedAt }),
    ...(finishedAt === undefined ? {} : { finished_at_unix_ms: finishedAt }),
    ...(errorCode === undefined ? {} : { error_code: errorCode }),
    ...(osError === undefined ? {} : { os_error: osError }),
  })
}

function parseResourceTaskPayload(value: unknown): ResourceTaskPayload {
  const payload = objectValue(value, "payload")
  const resource = stringValue(payload.resource, "payload.resource")
  if (resource !== "mount-archive" && resource !== "mount-ssh" && resource !== "unmount") {
    return invalid("payload.resource", "is not a supported resource task")
  }
  const sourcePath = payload.source_path === undefined
    ? undefined
    : displayString(payload.source_path, "payload.source_path", false, MAX_HEX_TEXT_BYTES)
  const mountPoint = payload.mount_point === undefined
    ? undefined
    : displayString(payload.mount_point, "payload.mount_point", false, MAX_HEX_TEXT_BYTES)
  const unmountPath = payload.unmount_path === undefined
    ? undefined
    : displayString(payload.unmount_path, "payload.unmount_path", false, MAX_HEX_TEXT_BYTES)
  const errorCode = payload.error_code === undefined
    ? undefined
    : boundedString(payload.error_code, "payload.error_code", MAX_ERROR_CODE_BYTES, false)
  const osError = payload.os_error === undefined ? undefined : integerValue(payload.os_error, "payload.os_error")
  return frozen({
    task_id: patternString(payload.task_id, "payload.task_id", UNSIGNED_DECIMAL_PATTERN, MAX_DECIMAL_TEXT_BYTES),
    command_sequence: integerValue(payload.command_sequence, "payload.command_sequence", 1),
    pane: parsePaneId(payload.pane, "payload.pane"),
    tab_id: patternString(payload.tab_id, "payload.tab_id", POSITIVE_DECIMAL_PATTERN, MAX_UINT64_TEXT_BYTES),
    resource,
    state: parsePreviewTaskState(payload.state, "payload.state"),
    ...(sourcePath === undefined ? {} : { source_path: sourcePath }),
    ...(mountPoint === undefined ? {} : { mount_point: mountPoint }),
    ...(unmountPath === undefined ? {} : { unmount_path: unmountPath }),
    ...(errorCode === undefined ? {} : { error_code: errorCode }),
    ...(osError === undefined ? {} : { os_error: osError }),
  })
}

function parseOpenPayload(value: unknown): OpenPayload {
  const payload = objectValue(value, "payload")
  const intent = stringValue(payload.intent, "payload.intent")
  if (intent !== "open" && intent !== "edit" && intent !== "preview") {
    return invalid("payload.intent", "is not a supported open intent")
  }
  const source = stringValue(payload.source, "payload.source")
  if (source !== "association" && source !== "platform") {
    return invalid("payload.source", "is not a supported open source")
  }
  if (payload.state !== "resolved") return invalid("payload.state", "must be resolved")
  if (!Array.isArray(payload.argv) || payload.argv.length === 0 || payload.argv.length > MAX_OPEN_ARGS) {
    return invalid("payload.argv", `must contain between 1 and ${MAX_OPEN_ARGS} arguments`)
  }
  const argv = payload.argv.map((argument, index) => boundedString(
    argument,
    `payload.argv[${index}]`,
    MAX_OPEN_ARG_BYTES,
    false,
  ))
  const pathBytesHex = patternString(payload.path_bytes_hex, "payload.path_bytes_hex", HEX_PATTERN)
  if (pathBytesHex.length === 0) return invalid("payload.path_bytes_hex", "must not be empty")
  return frozen({
    command_sequence: integerValue(payload.command_sequence, "payload.command_sequence", 1),
    intent,
    source,
    state: "resolved",
    path_bytes_hex: pathBytesHex,
    argv: frozen(argv),
  })
}

export function parseProtocolRecord(value: unknown): ProtocolRecord {
  const envelope = objectValue(value, "record")
  if (envelope.protocol !== PROTOCOL_NAME) {
    return invalid("record.protocol", `must equal ${PROTOCOL_NAME}`)
  }
  if (envelope.version !== PROTOCOL_VERSION && envelope.version !== WORKSPACE_PROTOCOL_VERSION && envelope.version !== SESSION_PROTOCOL_VERSION && envelope.version !== PREVIEW_SESSION_PROTOCOL_VERSION) {
    throw new ProtocolValidationError(
      `Unsupported NeoVifm protocol version: ${String(envelope.version)}`,
    )
  }

  const sequence = integerValue(envelope.sequence, "record.sequence", 0)
  const type = stringValue(envelope.type, "record.type")
  const version = envelope.version
  if (version === PREVIEW_SESSION_PROTOCOL_VERSION) {
    switch (type) {
      case "hello": return frozen({ protocol: PROTOCOL_NAME, version, type, sequence, payload: parseHelloPayload(envelope.payload) })
      case "workspace-snapshot": return frozen({ protocol: PROTOCOL_NAME, version, type, sequence, payload: parseSessionWorkspaceSnapshotPayload(envelope.payload) })
      case "task": return frozen({ protocol: PROTOCOL_NAME, version, type, sequence, payload: parsePreviewTaskPayload(envelope.payload) })
      case "preview": return frozen({ protocol: PROTOCOL_NAME, version, type, sequence, payload: parsePreviewPayload(envelope.payload) })
      case "action-task": return frozen({ protocol: PROTOCOL_NAME, version, type, sequence, payload: parseActionTaskPayload(envelope.payload) })
      case "resource-task": return frozen({ protocol: PROTOCOL_NAME, version, type, sequence, payload: parseResourceTaskPayload(envelope.payload) })
      case "open": return frozen({ protocol: PROTOCOL_NAME, version, type, sequence, payload: parseOpenPayload(envelope.payload) })
      case "command-error": return frozen({ protocol: PROTOCOL_NAME, version, type, sequence, payload: parseCommandErrorPayload(envelope.payload) })
      case "error": return frozen({ protocol: PROTOCOL_NAME, version, type, sequence, payload: parseErrorPayload(envelope.payload) })
      default: return invalid("record.type", `is unsupported for v3: ${type}`)
    }
  }
  if (version === SESSION_PROTOCOL_VERSION) {
    switch (type) {
      case "hello": return frozen({ protocol: PROTOCOL_NAME, version, type, sequence, payload: parseHelloPayload(envelope.payload) })
      case "workspace-snapshot": return frozen({ protocol: PROTOCOL_NAME, version, type, sequence, payload: parseSessionWorkspaceSnapshotPayload(envelope.payload) })
      case "command-error": return frozen({ protocol: PROTOCOL_NAME, version, type, sequence, payload: parseCommandErrorPayload(envelope.payload) })
      case "error": return frozen({ protocol: PROTOCOL_NAME, version, type, sequence, payload: parseErrorPayload(envelope.payload) })
      default: return invalid("record.type", `is unsupported for v2: ${type}`)
    }
  }
  if (version === WORKSPACE_PROTOCOL_VERSION) {
    switch (type) {
      case "hello":
        return frozen({
          protocol: PROTOCOL_NAME,
          version,
          type,
          sequence,
          payload: parseHelloPayload(envelope.payload),
        })
      case "workspace-snapshot":
        return frozen({
          protocol: PROTOCOL_NAME,
          version,
          type,
          sequence,
          payload: parseWorkspaceSnapshotPayload(envelope.payload),
        })
      case "error":
        return frozen({
          protocol: PROTOCOL_NAME,
          version,
          type,
          sequence,
          payload: parseErrorPayload(envelope.payload),
        })
      default:
        return invalid("record.type", `is unsupported for v1: ${type}`)
    }
  }
  switch (type) {
    case "hello":
      return frozen({
        protocol: PROTOCOL_NAME,
        version: PROTOCOL_VERSION,
        type,
        sequence,
        payload: parseHelloPayload(envelope.payload),
      })
    case "snapshot":
      return frozen({
        protocol: PROTOCOL_NAME,
        version: PROTOCOL_VERSION,
        type,
        sequence,
        payload: parseSnapshotPayload(envelope.payload),
      })
    case "error":
      return frozen({
        protocol: PROTOCOL_NAME,
        version: PROTOCOL_VERSION,
        type,
        sequence,
        payload: parseErrorPayload(envelope.payload),
      })
    default:
      return invalid("record.type", `is unsupported: ${type}`)
  }
}

function positiveLimit(
  value: number | undefined,
  name: string,
  defaultValue: number,
  maximum: number,
): number {
  const selected = value ?? defaultValue
  if (!Number.isSafeInteger(selected) || selected <= 0 || selected > maximum) {
    throw new RangeError(`${name} must be a positive safe integer no greater than ${maximum}`)
  }
  return selected
}

export class JsonlDecoder {
  readonly #maximumRecordBytes: number
  readonly #maximumTotalBytes: number
  readonly #maximumRecords: number
  #partialChunks: Uint8Array[] = []
  #partialBytes = 0
  #totalBytes = 0
  #recordCount = 0

  constructor(limits: JsonlLimits = {}) {
    this.#maximumRecordBytes = positiveLimit(
      limits.maximumRecordBytes,
      "maximumRecordBytes",
      MAX_PROTOCOL_RECORD_BYTES,
      MAX_PROTOCOL_RECORD_BYTES,
    )
    this.#maximumTotalBytes = positiveLimit(
      limits.maximumTotalBytes,
      "maximumTotalBytes",
      MAX_PROTOCOL_TOTAL_BYTES,
      Number.MAX_SAFE_INTEGER,
    )
    this.#maximumRecords = positiveLimit(
      limits.maximumRecords,
      "maximumRecords",
      MAX_PROTOCOL_RECORDS,
      1_000_000,
    )
    if (this.#maximumRecordBytes > this.#maximumTotalBytes) {
      throw new RangeError("maximumRecordBytes must not exceed maximumTotalBytes")
    }
  }

  push(chunk: string | Uint8Array): readonly ProtocolRecord[] {
    const bytes = typeof chunk === "string" ? UTF8_ENCODER.encode(chunk) : chunk
    this.#appendTotalBytes(bytes.byteLength)

    const records: ProtocolRecord[] = []
    let start = 0
    for (let index = 0; index < bytes.byteLength; index += 1) {
      if (bytes[index] !== 0x0a) {
        continue
      }

      const segment = bytes.subarray(start, index)
      records.push(this.#parseRecord(this.#consumeLine(segment)))
      start = index + 1
    }

    if (start < bytes.byteLength) {
      this.#appendPartial(bytes.subarray(start))
    }
    this.#assertRecordSize(this.#partialBytes)
    return records
  }

  flush(): readonly ProtocolRecord[] {
    if (this.#partialBytes !== 0) {
      throw new ProtocolValidationError("Incomplete JSONL record at end of stream")
    }
    return []
  }

  #appendTotalBytes(length: number): void {
    if (length > this.#maximumTotalBytes - this.#totalBytes) {
      throw new ProtocolValidationError(
        `JSONL stream exceeds maximumTotalBytes of ${this.#maximumTotalBytes} bytes`,
      )
    }
    this.#totalBytes += length
  }

  #appendPartial(segment: Uint8Array): void {
    this.#assertRecordSize(this.#partialBytes + segment.byteLength)
    this.#partialChunks.push(segment)
    this.#partialBytes += segment.byteLength
  }

  #consumeLine(segment: Uint8Array): Uint8Array {
    const length = this.#partialBytes + segment.byteLength
    this.#assertRecordSize(length)
    const chunks = this.#partialChunks
    this.#partialChunks = []
    this.#partialBytes = 0

    if (chunks.length === 0) {
      return segment
    }

    const line = new Uint8Array(length)
    let offset = 0
    for (const chunk of chunks) {
      line.set(chunk, offset)
      offset += chunk.byteLength
    }
    line.set(segment, offset)
    return line
  }

  #parseRecord(rawLine: Uint8Array): ProtocolRecord {
    if (this.#recordCount >= this.#maximumRecords) {
      throw new ProtocolValidationError(
        `JSONL stream exceeds maximumRecords of ${this.#maximumRecords} records`,
      )
    }
    this.#recordCount += 1
    const line = rawLine.byteLength > 0 && rawLine[rawLine.byteLength - 1] === 0x0d
      ? rawLine.subarray(0, rawLine.byteLength - 1)
      : rawLine
    let text: string
    try {
      text = new TextDecoder("utf-8", { fatal: true }).decode(line)
    } catch (error) {
      throw new ProtocolValidationError("JSONL record is not valid UTF-8", { cause: error })
    }
    if (text.trim().length === 0) {
      throw new ProtocolValidationError("Empty JSONL record")
    }
    try {
      return parseProtocolRecord(JSON.parse(text) as unknown)
    } catch (error) {
      if (error instanceof ProtocolValidationError) {
        throw error
      }
      throw new ProtocolValidationError(
        `Invalid JSONL record: ${error instanceof Error ? error.message : String(error)}`,
        { cause: error },
      )
    }
  }

  #assertRecordSize(length: number): void {
    if (length > this.#maximumRecordBytes) {
      throw new ProtocolValidationError(
        `JSONL record exceeds maximumRecordBytes of ${this.#maximumRecordBytes} bytes`,
      )
    }
  }
}
