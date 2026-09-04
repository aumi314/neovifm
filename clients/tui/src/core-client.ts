import {
  JsonlDecoder,
  type JsonlLimits,
  type ErrorRecord,
  type HelloPayload,
  type PaneSortKey,
  type ProtocolRecord,
  type SnapshotPayload,
  type WorkspaceSnapshotPayload,
} from "./protocol.js"

const DEFAULT_STDERR_LIMIT = 64 * 1024
const DEFAULT_TIMEOUT_MS = 30_000
const MAX_TIMEOUT_MS = 120_000

export type CoreClientErrorKind = "spawn" | "protocol" | "exit" | "core" | "timeout" | "cancelled"

interface CoreClientErrorDetails {
  readonly kind: CoreClientErrorKind
  readonly cause?: unknown
  readonly exitCode?: number
  readonly coreCode?: string
  readonly stderr?: string
  readonly stderrTruncated?: boolean
}

export class CoreClientError extends Error {
  override readonly name = "CoreClientError"
  readonly kind: CoreClientErrorKind
  readonly exitCode?: number
  readonly coreCode?: string
  readonly stderr: string
  readonly stderrTruncated: boolean

  constructor(message: string, details: CoreClientErrorDetails) {
    super(message, details.cause === undefined ? undefined : { cause: details.cause })
    this.kind = details.kind
    this.exitCode = details.exitCode
    this.coreCode = details.coreCode
    this.stderr = details.stderr ?? ""
    this.stderrTruncated = details.stderrTruncated ?? false
  }
}

export interface CoreProbeRequest {
  readonly executable: string
  readonly targetPath: string
  readonly rightPath?: string
  readonly cwd?: string
  readonly maximumRecordBytes?: number
  readonly maximumTotalBytes?: number
  readonly maximumStderrChars?: number
  readonly timeoutMs?: number
  readonly signal?: AbortSignal
  readonly onRecord?: (record: ProtocolRecord) => void
}

export interface CoreSnapshotProbeResult {
  readonly hello: HelloPayload
  readonly snapshot: SnapshotPayload
  readonly stderr: string
  readonly stderrTruncated: boolean
}

export interface CoreWorkspaceProbeResult {
  readonly hello: HelloPayload
  readonly workspace: WorkspaceSnapshotPayload
  readonly stderr: string
  readonly stderrTruncated: boolean
}

export type CoreProbeResult = CoreSnapshotProbeResult | CoreWorkspaceProbeResult

export interface CoreActionTarget {
  readonly path_bytes_hex: string
  readonly device: string
  readonly inode: string
  readonly ctime_unix_ns: string
  readonly kind: SnapshotPayload["entries"][number]["kind"]
}

export type CoreSessionCommand =
  | Readonly<{ action: "focus"; pane: "left" | "right" }>
  | Readonly<{ action: "focus-next" }>
  | Readonly<{ action: "move"; delta: -1 | 1 }>
  | Readonly<{ action: "move-to"; target: "first" | "last" }>
  | Readonly<{ action: "search"; query: string; direction: -1 | 1 }>
  | Readonly<{ action: "search-next"; direction: -1 | 1 }>
  | Readonly<{ action: "sort-cycle"; pane?: "left" | "right"; delta: -1 | 1 }>
  | Readonly<{ action: "sort-by"; pane: "left" | "right"; key: PaneSortKey }>
  | Readonly<{ action: "tab-cycle"; delta: number }>
  | Readonly<{ action: "new-tab"; pane: "left" | "right" }>
  | Readonly<{ action: "activate-tab" | "close-tab"; pane: "left" | "right"; tab_id: string }>
  | Readonly<{ action: "select-entry"; pane: "left" | "right"; index: number; toggle: boolean }>
  | Readonly<{
      action: "copy" | "move-files"
      pane: "left" | "right"
      cwd_bytes_hex: string
      snapshot_revision: string
      cwd_device: string
      cwd_inode: string
      cwd_ctime_unix_ns: string
      destination_cwd_bytes_hex: string
      destination_snapshot_revision: string
      destination_cwd_device: string
      destination_cwd_inode: string
      destination_cwd_ctime_unix_ns: string
      targets: readonly CoreActionTarget[]
    }>
  | Readonly<{
      action: "delete"
      pane: "left" | "right"
      cwd_bytes_hex: string
      snapshot_revision: string
      cwd_device: string
      cwd_inode: string
      cwd_ctime_unix_ns: string
      targets: readonly CoreActionTarget[]
    }>
  | Readonly<{
      action: "rename"
      pane: "left" | "right"
      cwd_bytes_hex: string
      snapshot_revision: string
      cwd_device: string
      cwd_inode: string
      cwd_ctime_unix_ns: string
      targets: readonly CoreActionTarget[]
      name: string
    }>
  | Readonly<{
      action: "mkdir"
      pane: "left" | "right"
      cwd_bytes_hex: string
      snapshot_revision: string
      cwd_device: string
      cwd_inode: string
      cwd_ctime_unix_ns: string
      name: string
    }>
  | Readonly<{ action: "undo" }>
  | Readonly<{ action: "cancel-action"; task_id: string }>
  | Readonly<{ action: "cancel-resource"; task_id: string }>
  | Readonly<{ action: "mount-ssh"; pane: "left" | "right"; remote: string }>
  | Readonly<{ action: "retry-action"; task_id: string }>
  | Readonly<{
      action: "preview"
      pane: "left" | "right"
      target_pane: "left" | "right"
      cwd_bytes_hex: string
      snapshot_revision: string
      cwd_device: string
      cwd_inode: string
      cwd_ctime_unix_ns: string
      path_bytes_hex: string
      device: string
      inode: string
      ctime_unix_ns: string
    }>
  | Readonly<{
      action: "open"
      intent: "open"
      pane: "left" | "right"
      cwd_bytes_hex: string
      snapshot_revision: string
      cwd_device: string
      cwd_inode: string
      cwd_ctime_unix_ns: string
      path_bytes_hex: string
      device: string
      inode: string
      ctime_unix_ns: string
      association_argv?: readonly string[]
    }>
  | Readonly<{ action: "enter" | "parent" | "toggle-selection" | "refresh" }>

export interface CoreSessionRequest {
  readonly executable: string
  readonly leftPath: string
  readonly rightPath: string
  readonly resume?: boolean
  readonly persist?: boolean
  readonly cwd?: string
  readonly signal?: AbortSignal
  readonly onRecord: (record: ProtocolRecord) => void
  readonly onError: (error: CoreClientError) => void
}

export interface CoreSession {
  readonly completion: Promise<void>
  send(command: CoreSessionCommand): Promise<boolean>
  close(): void
}

interface ProtocolConsumption {
  readonly records: readonly ProtocolRecord[]
  readonly error?: unknown
  readonly externallyAborted?: boolean
}

interface DiagnosticConsumption {
  readonly text: string
  readonly truncated: boolean
}

interface ProbeAbortContext {
  readonly signal: AbortSignal
  readonly abort: () => void
  readonly wasCancelled: () => boolean
  readonly wasTimedOut: () => boolean
  readonly dispose: () => void
}

function validateRequest(request: CoreProbeRequest): void {
  if (request.executable.length === 0 || request.executable.includes("\0")) {
    throw new CoreClientError("Core probe executable is invalid", { kind: "spawn" })
  }
  if (request.targetPath.length === 0 || request.targetPath.includes("\0")) {
    throw new CoreClientError("Target path is invalid", { kind: "spawn" })
  }
  if (request.rightPath !== undefined && (request.rightPath.length === 0 || request.rightPath.includes("\0"))) {
    throw new CoreClientError("Right target path is invalid", { kind: "spawn" })
  }
  if (request.cwd?.includes("\0")) {
    throw new CoreClientError("Core probe working directory is invalid", { kind: "spawn" })
  }
  if (request.maximumStderrChars !== undefined && (
    !Number.isSafeInteger(request.maximumStderrChars)
    || request.maximumStderrChars < 0
    || request.maximumStderrChars > DEFAULT_STDERR_LIMIT
  )) {
    throw new RangeError(`maximumStderrChars must be a safe integer from 0 to ${DEFAULT_STDERR_LIMIT}`)
  }
  if (
    request.timeoutMs !== undefined
    && (!Number.isSafeInteger(request.timeoutMs)
      || request.timeoutMs <= 0
      || request.timeoutMs > MAX_TIMEOUT_MS)
  ) {
    throw new RangeError(`timeoutMs must be a positive safe integer no greater than ${MAX_TIMEOUT_MS}`)
  }
}

function protocolLimits(request: CoreProbeRequest): JsonlLimits {
  const limits = {
    ...(request.maximumRecordBytes === undefined ? {} : { maximumRecordBytes: request.maximumRecordBytes }),
    ...(request.maximumTotalBytes === undefined ? {} : { maximumTotalBytes: request.maximumTotalBytes }),
  }
  new JsonlDecoder(limits)
  return limits
}

function createProbeAbortContext(signal: AbortSignal | undefined, timeoutMs: number): ProbeAbortContext {
  const controller = new AbortController()
  let cancelled = signal?.aborted ?? false
  let timedOut = false
  const cancel = () => {
    cancelled = true
    controller.abort()
  }
  signal?.addEventListener("abort", cancel, { once: true })
  const timeout = setTimeout(() => {
    timedOut = true
    controller.abort()
  }, timeoutMs)

  if (cancelled) {
    controller.abort()
  }

  return {
    signal: controller.signal,
    abort: () => controller.abort(),
    wasCancelled: () => cancelled,
    wasTimedOut: () => timedOut,
    dispose: () => {
      clearTimeout(timeout)
      signal?.removeEventListener("abort", cancel)
    },
  }
}

async function consumeProtocolStream(
  stream: ReadableStream<Uint8Array>,
  limits: JsonlLimits,
  onRecord: ((record: ProtocolRecord) => void) | undefined,
  abortProcess: () => void,
  abortSignal: AbortSignal,
  isExternallyAborted: () => boolean,
): Promise<ProtocolConsumption> {
  const decoder = new JsonlDecoder(limits)
  const reader = stream.getReader()
  let records: readonly ProtocolRecord[] = []
  let failure: unknown
  let failureWasExternallyAborted = false
  const fail = (error: unknown) => {
    if (failure !== undefined) {
      return
    }
    failure = error
    failureWasExternallyAborted = abortSignal.aborted && isExternallyAborted()
    if (!failureWasExternallyAborted) {
      abortProcess()
    }
  }
  const cancelReader = () => {
    void reader.cancel().catch(() => undefined)
  }
  abortSignal.addEventListener("abort", cancelReader, { once: true })
  if (abortSignal.aborted) {
    cancelReader()
  }

  try {
    for (;;) {
      const result = await reader.read()
      if (result.done) {
        break
      }
      if (failure === undefined) {
        try {
          for (const record of decoder.push(result.value)) {
            records = [...records, record]
            onRecord?.(record)
          }
        } catch (error) {
          fail(error)
        }
      }
      if (failure !== undefined) {
        break
      }
    }
    if (failure === undefined) {
      try {
        records = [...records, ...decoder.flush()]
      } catch (error) {
        fail(error)
      }
    }
  } catch (error) {
    if (failure === undefined && !abortSignal.aborted) {
      fail(error)
    }
  } finally {
    abortSignal.removeEventListener("abort", cancelReader)
    reader.releaseLock()
  }

  return failure === undefined
    ? { records }
    : { records, error: failure, externallyAborted: failureWasExternallyAborted }
}

function appendDiagnostic(
  current: DiagnosticConsumption,
  value: string,
  maximumChars: number,
): DiagnosticConsumption {
  const remaining = Math.max(0, maximumChars - current.text.length)
  return {
    text: `${current.text}${value.slice(0, remaining)}`,
    truncated: current.truncated || value.length > remaining,
  }
}

async function consumeDiagnosticStream(
  stream: ReadableStream<Uint8Array>,
  maximumChars: number,
  abortSignal: AbortSignal,
): Promise<DiagnosticConsumption> {
  const decoder = new TextDecoder("utf-8", { fatal: false })
  const reader = stream.getReader()
  let result: DiagnosticConsumption = { text: "", truncated: false }
  const cancelReader = () => {
    void reader.cancel().catch(() => undefined)
  }
  abortSignal.addEventListener("abort", cancelReader, { once: true })
  if (abortSignal.aborted) {
    cancelReader()
  }

  try {
    for (;;) {
      const chunk = await reader.read()
      if (chunk.done) {
        break
      }
      result = appendDiagnostic(result, decoder.decode(chunk.value, { stream: true }), maximumChars)
    }
    return appendDiagnostic(result, decoder.decode(), maximumChars)
  } catch (error) {
    if (abortSignal.aborted) {
      return result
    }
    throw error
  } finally {
    abortSignal.removeEventListener("abort", cancelReader)
    reader.releaseLock()
  }
}

function protocolFailure(
  message: string,
  cause: unknown,
  exitCode: number | undefined,
  diagnostic: DiagnosticConsumption,
): CoreClientError {
  return new CoreClientError(message, {
    kind: "protocol",
    cause,
    ...(exitCode === undefined ? {} : { exitCode }),
    stderr: diagnostic.text,
    stderrTruncated: diagnostic.truncated,
  })
}

function coreFailure(
  record: ErrorRecord,
  exitCode: number,
  diagnostic: DiagnosticConsumption,
): CoreClientError {
  return new CoreClientError(`Core probe failed: ${record.payload.message}`, {
    kind: "core",
    exitCode,
    coreCode: record.payload.code,
    stderr: diagnostic.text,
    stderrTruncated: diagnostic.truncated,
  })
}

function interpretResult(
  protocol: ProtocolConsumption,
  diagnostic: DiagnosticConsumption,
  exitCode: number,
): CoreProbeResult {
  if (protocol.error !== undefined) {
    throw protocolFailure("Core probe emitted an invalid protocol stream", protocol.error, exitCode, diagnostic)
  }
  const [hello, terminal, ...extra] = protocol.records
  if (hello?.type !== "hello" || terminal === undefined || extra.length !== 0) {
    throw protocolFailure("Core probe must emit hello followed by one terminal record", undefined, exitCode, diagnostic)
  }
  if (terminal.sequence <= hello.sequence) {
    throw protocolFailure("Core probe record sequence is not increasing", undefined, exitCode, diagnostic)
  }
  if (terminal.version !== hello.version) {
    throw protocolFailure("Core probe changed protocol version within a stream", undefined, exitCode, diagnostic)
  }
  if (terminal.type === "error") {
    throw coreFailure(terminal, exitCode, diagnostic)
  }
  if (exitCode !== 0) {
    throw new CoreClientError(`Core probe exited with status ${exitCode}`, {
      kind: "exit",
      exitCode,
      stderr: diagnostic.text,
      stderrTruncated: diagnostic.truncated,
    })
  }
  if (hello.version === 0 && hello.payload.capabilities.includes("snapshot-v0") && terminal.type === "snapshot") {
    return { hello: hello.payload, snapshot: terminal.payload, stderr: diagnostic.text, stderrTruncated: diagnostic.truncated }
  }
  if (hello.version === 1 && hello.payload.capabilities.includes("workspace-v1") && terminal.type === "workspace-snapshot") {
    return { hello: hello.payload, workspace: terminal.payload, stderr: diagnostic.text, stderrTruncated: diagnostic.truncated }
  }
  throw protocolFailure("Core probe terminal record does not match its advertised capability", undefined, exitCode, diagnostic)
}

function cancelledFailure(exitCode: number | undefined, diagnostic: DiagnosticConsumption): CoreClientError {
  return new CoreClientError("Core probe was cancelled", {
    kind: "cancelled",
    ...(exitCode === undefined ? {} : { exitCode }),
    stderr: diagnostic.text,
    stderrTruncated: diagnostic.truncated,
  })
}

function timeoutFailure(
  timeoutMs: number,
  exitCode: number | undefined,
  diagnostic: DiagnosticConsumption,
): CoreClientError {
  return new CoreClientError(`Core probe exceeded ${timeoutMs} ms`, {
    kind: "timeout",
    ...(exitCode === undefined ? {} : { exitCode }),
    stderr: diagnostic.text,
    stderrTruncated: diagnostic.truncated,
  })
}

export async function runCoreProbe(request: CoreProbeRequest): Promise<CoreProbeResult> {
  validateRequest(request)
  const limits = protocolLimits(request)
  const stderrLimit = request.maximumStderrChars ?? DEFAULT_STDERR_LIMIT
  const timeoutMs = request.timeoutMs ?? DEFAULT_TIMEOUT_MS
  if (request.signal?.aborted) {
    throw cancelledFailure(undefined, { text: "", truncated: false })
  }
  const abort = createProbeAbortContext(request.signal, timeoutMs)

  let process: Bun.Subprocess<"ignore", "pipe", "pipe">
  try {
    process = Bun.spawn({
      cmd: [request.executable, request.targetPath, ...(request.rightPath === undefined ? [] : [request.rightPath])],
      stdin: "ignore",
      stdout: "pipe",
      stderr: "pipe",
      signal: abort.signal,
      killSignal: "SIGKILL",
      ...(request.cwd === undefined ? {} : { cwd: request.cwd }),
    })
  } catch (cause) {
    abort.dispose()
    if (abort.wasCancelled()) {
      throw cancelledFailure(undefined, { text: "", truncated: false })
    }
    if (abort.wasTimedOut()) {
      throw timeoutFailure(timeoutMs, undefined, { text: "", truncated: false })
    }
    throw new CoreClientError(`Unable to start core probe: ${request.executable}`, {
      kind: "spawn",
      cause,
    })
  }

  let diagnostic: DiagnosticConsumption = { text: "", truncated: false }
  let exitCode: number | undefined
  try {
    const protocolPromise = consumeProtocolStream(
      process.stdout,
      limits,
      request.onRecord,
      abort.abort,
      abort.signal,
      () => abort.wasCancelled() || abort.wasTimedOut(),
    )
    const diagnosticPromise = consumeDiagnosticStream(process.stderr, stderrLimit, abort.signal)
    const exitPromise = process.exited
    const protocol = await protocolPromise
    if (protocol.error !== undefined) {
      void diagnosticPromise.catch(() => undefined)
      void exitPromise.catch(() => undefined)
      if (protocol.externallyAborted && abort.wasCancelled()) {
        throw cancelledFailure(undefined, diagnostic)
      }
      if (protocol.externallyAborted && abort.wasTimedOut()) {
        throw timeoutFailure(timeoutMs, undefined, diagnostic)
      }
      throw protocolFailure(
        "Core probe emitted an invalid protocol stream",
        protocol.error,
        undefined,
        diagnostic,
      )
    }

    const [receivedDiagnostic, receivedExitCode] = await Promise.all([
      diagnosticPromise,
      exitPromise,
    ])
    diagnostic = receivedDiagnostic
    exitCode = receivedExitCode
    if (abort.wasCancelled()) {
      throw cancelledFailure(exitCode, diagnostic)
    }
    if (abort.wasTimedOut()) {
      throw timeoutFailure(timeoutMs, exitCode, diagnostic)
    }
    return interpretResult(protocol, diagnostic, exitCode)
  } catch (cause) {
    if (cause instanceof CoreClientError) {
      throw cause
    }
    throw new CoreClientError("Core probe process failed", {
      kind: "exit",
      cause,
      ...(exitCode === undefined ? {} : { exitCode }),
      stderr: diagnostic.text,
      stderrTruncated: diagnostic.truncated,
    })
  } finally {
    abort.dispose()
  }
}

export function startCoreSession(request: CoreSessionRequest): CoreSession {
  if (request.executable.length === 0 || request.executable.includes("\0") || request.leftPath.length === 0 || request.rightPath.length === 0 || request.leftPath.includes("\0") || request.rightPath.includes("\0")) {
    throw new CoreClientError("Core session arguments are invalid", { kind: "spawn" })
  }
  const controller = new AbortController()
  const abort = () => controller.abort()
  request.signal?.addEventListener("abort", abort, { once: true })
  const sessionEnvironment = {
    ...Object.fromEntries(Object.entries(globalThis.process.env).filter((entry): entry is [string, string] => entry[1] !== undefined)),
    NEOVIFM_SESSION_RESUME: request.resume === true ? "1" : "0",
    NEOVIFM_SESSION_PERSIST: request.persist === true ? "1" : "0",
  }
  let process: Bun.Subprocess<"pipe", "pipe", "pipe">
  try {
    process = Bun.spawn({
      cmd: [request.executable, request.leftPath, request.rightPath],
      stdin: "pipe",
      stdout: "pipe",
      stderr: "pipe",
      signal: controller.signal,
      killSignal: "SIGKILL",
      env: sessionEnvironment,
      ...(request.cwd === undefined ? {} : { cwd: request.cwd }),
    })
  } catch (cause) {
    request.signal?.removeEventListener("abort", abort)
    throw new CoreClientError("Unable to start core session", { kind: "spawn", cause })
  }

  let commandSequence = 0
  let closed = false
  let gracefulShutdownTimer: ReturnType<typeof setTimeout> | undefined
  let writeQueue: Promise<void> = Promise.resolve()
  const fail = (message: string, cause?: unknown) => {
    if (!closed) controller.abort()
    request.onError(new CoreClientError(message, { kind: "protocol", cause }))
  }
  const completion = (async () => {
    const decoder = new JsonlDecoder({ maximumTotalBytes: 64 * 1024 * 1024, maximumRecords: 1_000_000 })
    const reader = process.stdout.getReader()
    const diagnostics = consumeDiagnosticStream(process.stderr, DEFAULT_STDERR_LIMIT, controller.signal)
    try {
      for (;;) {
        const result = await reader.read()
        if (result.done) break
        for (const record of decoder.push(result.value)) request.onRecord(record)
      }
      decoder.flush()
      const exitCode = await process.exited
      const diagnostic = await diagnostics
      if (!closed && exitCode !== 0) {
        request.onError(new CoreClientError(`Core session exited with status ${exitCode}`, {
          kind: "exit", exitCode, stderr: diagnostic.text, stderrTruncated: diagnostic.truncated,
        }))
      }
    } catch (cause) {
      if (!closed && !controller.signal.aborted) fail("Core session emitted an invalid protocol stream", cause)
    } finally {
      if (gracefulShutdownTimer !== undefined) clearTimeout(gracefulShutdownTimer)
      void diagnostics.catch(() => undefined)
      reader.releaseLock()
      request.signal?.removeEventListener("abort", abort)
    }
  })()
  return {
    completion,
    send(command) {
      if (closed || controller.signal.aborted || commandSequence >= Number.MAX_SAFE_INTEGER) return Promise.resolve(false)
      const nextSequence = commandSequence + 1
      const bytes = new TextEncoder().encode(`${JSON.stringify({ protocol: "neovifm-core", version: 3, type: "command", sequence: nextSequence, payload: command })}\n`)
      if (bytes.byteLength > 16 * 1024 + 1) return Promise.resolve(false)
      commandSequence = nextSequence
      const write = writeQueue.then(async () => {
        const written = await process.stdin.write(bytes)
        if (written !== bytes.byteLength) throw new Error(`short core command write: ${written}/${bytes.byteLength}`)
        await process.stdin.flush()
        return true
      }).catch((cause) => {
        if (!closed && !controller.signal.aborted) fail("Failed to write core session command", cause)
        return false
      })
      writeQueue = write.then(() => undefined)
      return write
    },
    close() {
      if (closed) return
      closed = true
      void writeQueue.then(() => {
        process.stdin.end()
        gracefulShutdownTimer = setTimeout(() => process.kill("SIGKILL"), 3_000)
      }).catch(() => controller.abort())
    },
  }
}
