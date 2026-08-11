import { afterEach, expect, test } from "bun:test"
import { mkdir, mkdtemp, rename, rm, unlink, writeFile } from "node:fs/promises"
import { tmpdir } from "node:os"
import { resolve } from "node:path"

import { startCoreSession, type CoreSession } from "../src/core-client.js"
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

async function waitFor(predicate: () => boolean, diagnostic: () => string): Promise<void> {
  const deadline = Date.now() + 15_000
  while (!predicate()) {
    if (Date.now() >= deadline) throw new Error(`timed out waiting for watcher: ${diagnostic()}`)
    await Bun.sleep(10)
  }
}

function startTracked(left: string, right: string): {
  readonly session: CoreSession
  readonly state: () => ProbeState
  readonly records: unknown[]
  readonly errors: Error[]
} {
  const executable = process.env.NEOVIFM_CORE_SESSION
  if (executable === undefined || executable.length === 0) {
    throw new Error("NEOVIFM_CORE_SESSION must point to the built core session")
  }
  let current: ProbeState = initialProbeState()
  const records: unknown[] = []
  const errors: Error[] = []
  const session = startCoreSession({
    executable,
    leftPath: left,
    rightPath: right,
    onRecord: (record) => { records.push(record); current = reduceProbeState(current, record) },
    onError: (error) => errors.push(error),
  })
  sessions.add(session)
  return { session, state: () => current, records, errors }
}

function watchRecords(records: readonly unknown[]): Array<Record<string, unknown>> {
  return records.filter((record): record is Record<string, unknown> => {
    if (typeof record !== "object" || record === null) return false
    const payload = (record as { payload?: unknown }).payload
    return typeof payload === "object" && payload !== null
      && (payload as { trigger?: unknown }).trigger === "watch"
  })
}

test("real core watches external file changes and refreshes the active preview", async () => {
  root = await mkdtemp(resolve(tmpdir(), "neovifm-watcher-"))
  const leftBase = resolve(root, "左侧 watcher path")
  const right = resolve(root, "right")
  await Promise.all([mkdir(leftBase, { recursive: true }), mkdir(right)])
  let left = leftBase
  if (process.platform === "win32") {
    for (let index = 0; left.length <= 280; index += 1) left = resolve(left, `深层目录-${index.toString().padStart(2, "0")}`)
    await mkdir(left, { recursive: true })
  }
  const selected = resolve(left, "a-selected-中文.txt")
  await Promise.all([
    writeFile(selected, "first preview"),
    writeFile(resolve(right, "right.txt"), "right"),
  ])

  const running = startTracked(left, right)
  const diagnostic = () => JSON.stringify({
    state: running.state(),
    records: running.records.slice(-6),
    errors: running.errors.map(String),
  })
  await waitFor(() => {
    const state = running.state()
    return state.phase === "ready" && "session" in state && state.preview?.content === "first preview"
  }, diagnostic)

  const created = resolve(left, "b-created.txt")
  await writeFile(created, "created")
  await waitFor(() => {
    const state = running.state()
    return state.phase === "ready" && "session" in state
      && state.workspace.left.entries.some((entry) => entry.name_display === "b-created.txt")
      && watchRecords(running.records).length >= 1
  }, diagnostic)

  await writeFile(selected, "updated preview")
  await waitFor(() => {
    const state = running.state()
    return state.phase === "ready" && "session" in state && state.preview?.content === "updated preview"
  }, diagnostic)

  const renamed = resolve(left, "c-renamed.txt")
  await rename(created, renamed)
  await waitFor(() => {
    const state = running.state()
    return state.phase === "ready" && "session" in state
      && state.workspace.left.entries.some((entry) => entry.name_display === "c-renamed.txt")
      && !state.workspace.left.entries.some((entry) => entry.name_display === "b-created.txt")
  }, diagnostic)
  await unlink(renamed)
  await waitFor(() => {
    const state = running.state()
    return state.phase === "ready" && "session" in state
      && !state.workspace.left.entries.some((entry) => entry.name_display === "c-renamed.txt")
  }, diagnostic)

  const subdirectory = resolve(left, "watched-subdirectory")
  await mkdir(subdirectory)
  await waitFor(() => {
    const state = running.state()
    return state.phase === "ready" && "session" in state
      && state.workspace.left.entries.some((entry) => entry.name_display === "watched-subdirectory")
  }, diagnostic)
  let state = running.state()
  if (!(state.phase === "ready" && "session" in state)) throw new Error("expected ready watcher state")
  const subdirectoryIndex = state.workspace.left.entries.findIndex((entry) => entry.name_display === "watched-subdirectory")
  expect(await running.session.send({ action: "select-entry", pane: "left", index: subdirectoryIndex, toggle: false })).toBe(true)
  expect(await running.session.send({ action: "enter" })).toBe(true)
  await waitFor(() => {
    const current = running.state()
    return current.phase === "ready" && "session" in current
      && current.workspace.left.cwd_display.endsWith("watched-subdirectory")
  }, diagnostic)
  await writeFile(resolve(subdirectory, "after-rebind.txt"), "rebound")
  await waitFor(() => {
    const current = running.state()
    return current.phase === "ready" && "session" in current
      && current.workspace.left.entries.some((entry) => entry.name_display === "after-rebind.txt")
  }, diagnostic)

  state = running.state()
  if (!(state.phase === "ready" && "session" in state)) throw new Error("expected final watcher state")
  expect(running.errors).toEqual([])
  expect(watchRecords(running.records).length).toBeGreaterThanOrEqual(5)
  const latestWatch = watchRecords(running.records).at(-1)
  expect((latestWatch?.payload as { command_sequence?: unknown }).command_sequence).toBe(2)
  running.session.close()
  await running.session.completion
  sessions.delete(running.session)
}, { timeout: 60_000 })
