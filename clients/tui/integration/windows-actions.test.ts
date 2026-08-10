import { afterEach, expect, test } from "bun:test"
import { mkdir, mkdtemp, readFile, rm, symlink, writeFile } from "node:fs/promises"
import { tmpdir } from "node:os"
import { resolve } from "node:path"

import { startCoreSession, type CoreSession, type CoreSessionCommand } from "../src/core-client.js"
import { initialProbeState, reduceProbeState, type ProbeState } from "../src/probe-state.js"

let root: string | undefined
const sessions = new Set<CoreSession>()
const additionalRoots = new Set<string>()

afterEach(async () => {
  for (const session of sessions) {
    session.close()
    await session.completion
  }
  sessions.clear()
  if (root !== undefined) await rm(root, { recursive: true, force: true })
  for (const path of additionalRoots) await rm(path, { recursive: true, force: true })
  additionalRoots.clear()
  root = undefined
  delete process.env.NEOVIFM_TRASH_EXECUTABLE
})

async function waitFor(predicate: () => boolean, timeoutMs = 20_000, diagnostic?: () => unknown): Promise<void> {
  const deadline = Date.now() + timeoutMs
  while (!predicate()) {
    if (Date.now() >= deadline) throw new Error(`timed out waiting for Windows file action: ${JSON.stringify(diagnostic?.())}`)
    await Bun.sleep(10)
  }
}

function start(left: string, right: string): {
  readonly session: CoreSession
  readonly state: () => ProbeState
  readonly errors: Error[]
} {
  const executable = process.env.NEOVIFM_CORE_SESSION
  if (executable === undefined || executable.length === 0) throw new Error("NEOVIFM_CORE_SESSION must point to the built Windows core")
  let state: ProbeState = initialProbeState()
  const errors: Error[] = []
  const session = startCoreSession({
    executable,
    leftPath: left,
    rightPath: right,
    onRecord: (record) => { state = reduceProbeState(state, record) },
    onError: (error) => errors.push(error),
  })
  sessions.add(session)
  return { session, state: () => state, errors }
}

function commandFor(state: ProbeState, action: "copy" | "move-files" | "delete", name: string): CoreSessionCommand {
  if (state.phase !== "ready" || !("session" in state)) throw new Error("expected ready Windows action session")
  const source = state.workspace.left
  const entry = source.entries.find((candidate) => candidate.name_display === name)
  if (entry === undefined || source.cwd_device === undefined || source.cwd_inode === undefined || source.cwd_ctime_unix_ns === undefined || entry.device === undefined || entry.inode === undefined || entry.ctime_unix_ns === undefined) {
    throw new Error(`expected identity for ${name}`)
  }
  const common = {
    pane: "left" as const,
    cwd_bytes_hex: source.cwd_bytes_hex,
    snapshot_revision: source.snapshot_revision,
    cwd_device: source.cwd_device,
    cwd_inode: source.cwd_inode,
    cwd_ctime_unix_ns: source.cwd_ctime_unix_ns,
    targets: [{ path_bytes_hex: entry.path_bytes_hex, device: entry.device, inode: entry.inode, ctime_unix_ns: entry.ctime_unix_ns, kind: entry.kind }],
  }
  if (action === "delete") return { action, ...common }
  return {
    action,
    ...common,
    destination_cwd_bytes_hex: state.workspace.right.cwd_bytes_hex,
    destination_snapshot_revision: state.workspace.right.snapshot_revision,
    destination_cwd_device: state.workspace.right.cwd_device!,
    destination_cwd_inode: state.workspace.right.cwd_inode!,
    destination_cwd_ctime_unix_ns: state.workspace.right.cwd_ctime_unix_ns!,
  }
}

function terminal(state: ProbeState, sequence: number, expected: "done" | "failed"): boolean {
  return state.phase === "ready" && "session" in state
    && state.actionTasks?.some((task) => task.command_sequence === sequence && task.state === expected) === true
}

type SessionProbeState = Extract<ProbeState, { readonly session: true }>

function sessionState(state: ProbeState): SessionProbeState {
  if (state.phase === "ready" && "session" in state) return state
  throw new Error("expected ready Windows action session")
}

test.skipIf(process.platform !== "win32")("Windows core performs copy, move, mkdir and undo without overwriting", async () => {
  root = await mkdtemp(resolve(tmpdir(), "neovifm-windows-actions-"))
  const left = resolve(root, "左侧")
  const right = resolve(root, "右侧")
  await Promise.all([mkdir(left), mkdir(right)])
  await writeFile(resolve(left, "note.txt"), "source")

  const running = start(left, right)
  await waitFor(() => running.state().phase === "ready")
  expect(sessionState(running.state()).hello.capabilities.includes("file-actions-v1")).toBe(true)

  let state = sessionState(running.state())
  let sequence = state.commandSequence + 1
  expect(await running.session.send(commandFor(state, "copy", "note.txt"))).toBe(true)
  await waitFor(() => terminal(running.state(), sequence, "done") || terminal(running.state(), sequence, "failed"),
    20_000, () => ({ state: running.state(), errors: running.errors.map(String) }))
  expect(sessionState(running.state()).actionTasks?.find((task) => task.command_sequence === sequence))
    .toMatchObject({ state: "done" })
  expect(await readFile(resolve(right, "note.txt"), "utf8")).toBe("source")

  state = sessionState(running.state())
  sequence = state.commandSequence + 1
  expect(await running.session.send({ action: "undo" })).toBe(true)
  await waitFor(() => {
    const current = running.state()
    return current.phase === "ready" && "session" in current
      && current.commandSequence === sequence
      && !current.workspace.right.entries.some((entry) => entry.name_display === "note.txt")
  }, 20_000,
  () => ({ state: running.state(), errors: running.errors.map(String) }))

  state = sessionState(running.state())
  sequence = state.commandSequence + 1
  expect(await running.session.send(commandFor(state, "move-files", "note.txt"))).toBe(true)
  await waitFor(() => terminal(running.state(), sequence, "done") || terminal(running.state(), sequence, "failed"),
    20_000, () => ({ state: running.state(), errors: running.errors.map(String) }))
  expect(sessionState(running.state()).actionTasks?.find((task) => task.command_sequence === sequence))
    .toMatchObject({ state: "done" })
  expect(await readFile(resolve(right, "note.txt"), "utf8")).toBe("source")

  state = sessionState(running.state())
  sequence = state.commandSequence + 1
  expect(await running.session.send({ action: "undo" })).toBe(true)
  await waitFor(() => {
    const current = running.state()
    return current.phase === "ready" && "session" in current
      && current.commandSequence === sequence
      && current.workspace.left.entries.some((entry) => entry.name_display === "note.txt")
  })

  state = sessionState(running.state())
  sequence = state.commandSequence + 1
  expect(await running.session.send({
    action: "mkdir",
    pane: "left",
    cwd_bytes_hex: state.workspace.left.cwd_bytes_hex,
    snapshot_revision: state.workspace.left.snapshot_revision,
    cwd_device: state.workspace.left.cwd_device!,
    cwd_inode: state.workspace.left.cwd_inode!,
    cwd_ctime_unix_ns: state.workspace.left.cwd_ctime_unix_ns!,
    name: "新目录",
  })).toBe(true)
  await waitFor(() => terminal(running.state(), sequence, "done"))
  expect(sessionState(running.state()).workspace.left.entries
    .some((entry) => entry.name_display === "新目录")).toBe(true)

  state = sessionState(running.state())
  sequence = state.commandSequence + 1
  expect(await running.session.send({ action: "undo" })).toBe(true)
  await waitFor(() => {
    const current = running.state()
    return current.phase === "ready" && "session" in current
      && current.commandSequence === sequence
      && !current.workspace.left.entries.some((entry) => entry.name_display === "新目录")
  })

  await writeFile(resolve(right, "note.txt"), "existing")
  state = sessionState(running.state())
  sequence = state.commandSequence + 1
  expect(await running.session.send(commandFor(state, "copy", "note.txt"))).toBe(true)
  await waitFor(() => terminal(running.state(), sequence, "failed"))
  expect(await readFile(resolve(right, "note.txt"), "utf8")).toBe("existing")
  expect(running.errors).toEqual([])
}, { timeout: 60_000 })

test.skipIf(process.platform !== "win32")("Windows delete uses the Recycle Bin and restores after helper failure", async () => {
  root = await mkdtemp(resolve(tmpdir(), "neovifm-windows-delete-"))
  const left = resolve(root, "left")
  const right = resolve(root, "right")
  await Promise.all([mkdir(left), mkdir(right)])
  await writeFile(resolve(left, "delete-me.txt"), "delete")

  let running = start(left, right)
  await waitFor(() => running.state().phase === "ready")
  let state = sessionState(running.state())
  let sequence = state.commandSequence + 1
  expect(await running.session.send(commandFor(state, "delete", "delete-me.txt"))).toBe(true)
  await waitFor(() => terminal(running.state(), sequence, "done") || terminal(running.state(), sequence, "failed"), 30_000,
    () => ({ state: running.state(), errors: running.errors.map(String) }))
  expect(sessionState(running.state()).actionTasks?.find((task) => task.command_sequence === sequence))
    .toMatchObject({ state: "done" })
  expect(!sessionState(running.state()).workspace.left.entries
    .some((entry) => entry.name_display === "delete-me.txt")).toBe(true)
  running.session.close()
  await running.session.completion
  sessions.delete(running.session)

  await writeFile(resolve(left, "restore-me.txt"), "restore")
  process.env.NEOVIFM_TRASH_EXECUTABLE = resolve(process.env.SystemRoot ?? "C:\\Windows", "System32", "where.exe")
  running = start(left, right)
  await waitFor(() => running.state().phase === "ready")
  state = sessionState(running.state())
  sequence = state.commandSequence + 1
  expect(await running.session.send(commandFor(state, "delete", "restore-me.txt"))).toBe(true)
  await waitFor(() => terminal(running.state(), sequence, "failed"), 30_000)
  expect(await readFile(resolve(left, "restore-me.txt"), "utf8")).toBe("restore")
  expect(running.errors).toEqual([])
}, { timeout: 60_000 })

test.skipIf(process.platform !== "win32")("Windows actions handle long Unicode paths and refuse junction recursion", async () => {
  root = await mkdtemp(resolve(tmpdir(), "neovifm-windows-long-"))
  let longRoot = root
  for (let index = 0; index < 8; index += 1) longRoot = resolve(longRoot, `层级-${index}-${"x".repeat(28)}`)
  const left = resolve(longRoot, "左侧")
  const right = resolve(longRoot, "右侧")
  const outside = resolve(root, "outside")
  await Promise.all([mkdir(left, { recursive: true }), mkdir(right, { recursive: true }), mkdir(outside)])
  await writeFile(resolve(left, "长路径.txt"), "long")
  await mkdir(resolve(left, "树目录", "内层"), { recursive: true })
  await writeFile(resolve(left, "树目录", "内层", "数据.txt"), "nested")
  await writeFile(resolve(outside, "sentinel.txt"), "sentinel")
  await symlink(outside, resolve(left, "junction"), "junction")

  const running = start(left, right)
  await waitFor(() => running.state().phase === "ready")
  let state = sessionState(running.state())
  let sequence = state.commandSequence + 1
  expect(await running.session.send(commandFor(state, "copy", "长路径.txt"))).toBe(true)
  await waitFor(() => terminal(running.state(), sequence, "done"))
  expect(await readFile(resolve(right, "长路径.txt"), "utf8")).toBe("long")

  state = sessionState(running.state())
  sequence = state.commandSequence + 1
  expect(await running.session.send(commandFor(state, "copy", "树目录"))).toBe(true)
  await waitFor(() => terminal(running.state(), sequence, "done"))
  expect(await readFile(resolve(right, "树目录", "内层", "数据.txt"), "utf8")).toBe("nested")

  state = sessionState(running.state())
  sequence = state.commandSequence + 1
  expect(await running.session.send(commandFor(state, "copy", "junction"))).toBe(true)
  await waitFor(() => terminal(running.state(), sequence, "failed"))
  expect(await readFile(resolve(outside, "sentinel.txt"), "utf8")).toBe("sentinel")
  expect(running.errors).toEqual([])
}, { timeout: 60_000 })

const crossVolumeSourceRoot = process.env.NEOVIFM_CROSS_VOLUME_SOURCE_ROOT
const crossVolumeDestinationRoot = process.env.NEOVIFM_CROSS_VOLUME_DESTINATION_ROOT

test.skipIf(process.platform !== "win32" || crossVolumeSourceRoot === undefined || crossVolumeDestinationRoot === undefined)(
  "Windows move refuses a real cross-volume copy-delete fallback",
  async () => {
    const sourceBase = await mkdtemp(resolve(crossVolumeSourceRoot!, "source-"))
    const destinationBase = await mkdtemp(resolve(crossVolumeDestinationRoot!, "destination-"))
    additionalRoots.add(sourceBase)
    additionalRoots.add(destinationBase)
    await writeFile(resolve(sourceBase, "stay.txt"), "source")

    const running = start(sourceBase, destinationBase)
    await waitFor(() => running.state().phase === "ready")
    const state = sessionState(running.state())
    const sequence = state.commandSequence + 1
    expect(await running.session.send(commandFor(state, "move-files", "stay.txt"))).toBe(true)
    await waitFor(() => terminal(running.state(), sequence, "failed"))
    expect(sessionState(running.state()).actionTasks?.find((task) => task.command_sequence === sequence))
      .toMatchObject({ state: "failed", error_code: "cross-filesystem-move-unsupported" })
    expect(await readFile(resolve(sourceBase, "stay.txt"), "utf8")).toBe("source")
    await expect(readFile(resolve(destinationBase, "stay.txt"), "utf8")).rejects.toThrow()
    expect(running.errors).toEqual([])
  },
  { timeout: 60_000 },
)
