import { afterEach, expect, test } from "bun:test"
import { mkdir, mkdtemp, readFile, rm, writeFile } from "node:fs/promises"
import { tmpdir } from "node:os"
import { resolve } from "node:path"

import { runCoreProbe, startCoreSession, type CoreSession } from "../src/core-client.js"
import { initialProbeState, reduceProbeState, type ProbeState } from "../src/probe-state.js"

let root: string | undefined
const sessions = new Set<CoreSession>()

afterEach(async () => {
  for (const session of sessions) {
    session.close()
    await session.completion
  }
  sessions.clear()
  if (root !== undefined) await rm(root, { recursive: true, force: true })
  root = undefined
})

async function waitFor(predicate: () => boolean, timeoutMs = 15_000, diagnostic?: () => string): Promise<void> {
  const deadline = Date.now() + timeoutMs
  while (!predicate()) {
    if (Date.now() >= deadline) {
      const details = diagnostic?.()
      throw new Error(`timed out waiting for Windows core session${details === undefined ? "" : `: ${details}`}`)
    }
    await Bun.sleep(10)
  }
}

function executable(name: "NEOVIFM_CORE_PROBE" | "NEOVIFM_CORE_SESSION"): string {
  const value = process.env[name]
  if (value === undefined || value.length === 0) throw new Error(`${name} must point to a built Windows core`)
  return value
}

function startTracked(left: string, right: string, options: {
  readonly resume?: boolean
  readonly persist?: boolean
} = {}): { readonly session: CoreSession; readonly state: () => ProbeState; readonly errors: Error[]; readonly records: unknown[] } {
  let current: ProbeState = initialProbeState()
  const errors: Error[] = []
  const records: unknown[] = []
  const session = startCoreSession({
    executable: executable("NEOVIFM_CORE_SESSION"),
    leftPath: left,
    rightPath: right,
    resume: options.resume,
    persist: options.persist,
    onRecord: (record) => { records.push(record); current = reduceProbeState(current, record) },
    onError: (error) => errors.push(error),
  })
  sessions.add(session)
  return { session, state: () => current, errors, records }
}

async function closeTracked(session: CoreSession): Promise<void> {
  session.close()
  await session.completion
  sessions.delete(session)
}

test.skipIf(process.platform !== "win32")("real Windows cores handle Unicode paths and publish idle preview events", async () => {
  root = await mkdtemp(resolve(tmpdir(), "neovifm-windows-"))
  const left = resolve(root, "左侧目录")
  const right = resolve(root, "右侧目录")
  await Promise.all([mkdir(left), mkdir(right)])
  await writeFile(resolve(left, "a-阿尔法.txt"), "alpha")
  await writeFile(resolve(left, "b-贝塔.txt"), "beta")
  await writeFile(resolve(right, "右侧.txt"), "right")

  const probe = await runCoreProbe({
    executable: executable("NEOVIFM_CORE_PROBE"),
    targetPath: left,
    rightPath: right,
  })
  if (!("workspace" in probe)) throw new Error("expected Windows workspace probe")
  expect(probe.workspace.left.entries.some((entry) => entry.name_display === "a-阿尔法.txt")).toBe(true)

  const running = startTracked(left, right)
  await waitFor(() => {
    const state = running.state()
    return state.phase === "ready" && "session" in state && state.preview?.content === "alpha"
  }, 15_000, () => JSON.stringify({ state: running.state(), records: running.records.slice(-4), errors: running.errors.map(String) }))
  let state = running.state()
  if (!(state.phase === "ready" && "session" in state)) throw new Error("expected ready Windows session")
  expect(state.hello.capabilities).toContain("file-actions-v1")
  expect(BigInt(state.workspace.left.cwd_device!)).toBeGreaterThan(0n)
  expect(BigInt(state.workspace.left.cwd_inode!)).toBeGreaterThan(0n)
  expect(BigInt(state.workspace.left.cwd_ctime_unix_ns!)).toBeGreaterThan(0n)
  const alpha = state.workspace.left.entries.find((entry) => entry.name_display === "a-阿尔法.txt")
  expect(BigInt(alpha!.device!)).toBeGreaterThan(0n)
  expect(BigInt(alpha!.inode!)).toBeGreaterThan(0n)
  expect(BigInt(alpha!.ctime_unix_ns!)).toBeGreaterThan(0n)

  expect(await running.session.send({ action: "search", query: "贝塔", direction: 1 })).toBe(true)
  await waitFor(() => {
    const current = running.state()
    return current.phase === "ready" && "session" in current
      && current.workspace.left.entries[current.workspace.left.cursor]?.name_display === "b-贝塔.txt"
  })
  expect(await running.session.send({ action: "focus", pane: "right" })).toBe(true)
  expect(await running.session.send({ action: "new-tab", pane: "right" })).toBe(true)
  expect(await running.session.send({ action: "sort-by", pane: "right", key: "size" })).toBe(true)
  await writeFile(resolve(right, "刷新.txt"), "refresh")
  expect(await running.session.send({ action: "refresh" })).toBe(true)
  await waitFor(() => {
    const current = running.state()
    return current.phase === "ready" && "session" in current
      && current.workspace.active_pane === "right"
      && current.workspace.right_tabs?.length === 2
      && current.workspace.right.entries.some((entry) => entry.name_display === "刷新.txt")
  })
  expect(running.errors).toEqual([])
  await closeTracked(running.session)
}, { timeout: 45_000 })

test.skipIf(process.platform !== "win32")("Windows default state path restores and replaces an existing Unicode session", async () => {
  root = await mkdtemp(resolve(tmpdir(), "neovifm-windows-state-"))
  const left = resolve(root, "左")
  const right = resolve(root, "右")
  const localAppData = resolve(root, "本地状态")
  await Promise.all([mkdir(left), mkdir(right), mkdir(localAppData)])
  await Promise.all([
    writeFile(resolve(left, "left.txt"), "left"),
    writeFile(resolve(right, "right.txt"), "right"),
  ])

  const previousState = process.env.NEOVIFM_SESSION_STATE
  const previousLocalAppData = process.env.LOCALAPPDATA
  delete process.env.NEOVIFM_SESSION_STATE
  process.env.LOCALAPPDATA = localAppData
  try {
    const first = startTracked(left, right, { persist: true })
    await waitFor(() => first.state().phase === "ready", 15_000,
      () => JSON.stringify({ phase: first.state().phase, records: first.records.slice(0, 4), errors: first.errors.map(String) }))
    expect(await first.session.send({ action: "focus", pane: "right" })).toBe(true)
    expect(await first.session.send({ action: "new-tab", pane: "right" })).toBe(true)
    await waitFor(() => {
      const state = first.state()
      return state.phase === "ready" && "session" in state
        && state.workspace.active_pane === "right" && state.workspace.right_tabs?.length === 2
    })
    await closeTracked(first.session)

    const statePath = resolve(localAppData, "neovifm", "session.json")
    expect(JSON.parse(await readFile(statePath, "utf8")).version).toBe(1)

    const second = startTracked(left, right, { resume: true, persist: true })
    await waitFor(() => {
      const state = second.state()
      return state.phase === "ready" && "session" in state
        && state.workspace.active_pane === "right" && state.workspace.right_tabs?.length === 2
    }, 15_000, () => JSON.stringify({ state: second.state(), records: second.records.slice(-4), errors: second.errors.map(String) }))
    expect(await second.session.send({ action: "focus", pane: "left" })).toBe(true)
    await waitFor(() => {
      const state = second.state()
      return state.phase === "ready" && "session" in state && state.workspace.active_pane === "left"
    })
    await closeTracked(second.session)

    const third = startTracked(left, right, { resume: true })
    await waitFor(() => {
      const state = third.state()
      return state.phase === "ready" && "session" in state && state.workspace.active_pane === "left"
    })
    expect(third.errors).toEqual([])
    await closeTracked(third.session)
  } finally {
    if (previousState === undefined) delete process.env.NEOVIFM_SESSION_STATE
    else process.env.NEOVIFM_SESSION_STATE = previousState
    if (previousLocalAppData === undefined) delete process.env.LOCALAPPDATA
    else process.env.LOCALAPPDATA = previousLocalAppData
  }
}, { timeout: 45_000 })
