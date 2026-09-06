import { afterEach, expect, test } from "bun:test"
import { mkdtemp, mkdir, rm, writeFile } from "node:fs/promises"
import { tmpdir } from "node:os"
import { resolve } from "node:path"
import { createSignal } from "solid-js"
import { testRender } from "@opentui/solid"

import { App } from "../src/app.js"
import { startCoreSession } from "../src/core-client.js"
import { initialProbeState, reduceProbeState, type ProbeState } from "../src/probe-state.js"

let root: string | undefined

afterEach(async () => {
  if (root !== undefined) await rm(root, { recursive: true })
  root = undefined
})

async function waitFor(predicate: () => boolean, timeoutMs = 10_000): Promise<void> {
  const deadline = Date.now() + timeoutMs
  while (!predicate()) {
    if (Date.now() >= deadline) throw new Error("timed out waiting for rename session update")
    await Bun.sleep(10)
  }
}

test.skipIf(process.platform === "win32")("real session renames through cw, reports conflicts, and undoes", async () => {
  const executable = process.env.NEOVIFM_CORE_SESSION
  if (executable === undefined || executable.length === 0) {
    throw new Error("NEOVIFM_CORE_SESSION must point to the built core session")
  }
  root = await mkdtemp(resolve(tmpdir(), "neovifm-rename-"))
  const left = resolve(root, "left")
  const right = resolve(root, "right")
  await mkdir(left)
  await mkdir(right)
  await writeFile(resolve(left, "note.txt"), "note")
  await writeFile(resolve(left, "conflict.txt"), "conflict")

  const [state, setState] = createSignal<ProbeState>(initialProbeState())
  const errors: Error[] = []
  const session = startCoreSession({
    executable,
    leftPath: left,
    rightPath: right,
    onRecord: (record) => setState((previous) => reduceProbeState(previous, record)),
    onError: (error) => errors.push(error),
  })
  const appProps = () => {
    const current = state()
    return {
      workspace: current.phase === "ready" && "workspace" in current ? current.workspace : undefined,
      capabilities: current.phase === "ready" ? current.hello.capabilities : undefined,
      onCommand: (command: Parameters<typeof session.send>[0]) => session.send(command),
    }
  }
  const setup = await testRender(() => <App {...appProps()} />, { width: 100, height: 20 })

  try {
    await waitFor(() => {
      const current = state()
      return current.phase === "ready" && "workspace" in current
        && current.workspace.left.entries.some((entry) => entry.name_display === "note.txt")
    })
    await setup.renderOnce()
    const capabilityFrame = state()
    if (capabilityFrame.phase !== "ready") throw new Error("session not ready")
    expect(capabilityFrame.hello.capabilities).toContain("file-rename-v1")

    // Name-sorted: conflict.txt at 0, note.txt at 1.
    setup.mockInput.pressKey("j")
    await waitFor(() => {
      const current = state()
      return current.phase === "ready" && "workspace" in current
        && current.workspace.left.entries[current.workspace.left.cursor]?.name_display === "note.txt"
    })

    setup.mockInput.pressKey("c")
    setup.mockInput.pressKey("w")
    await setup.renderOnce()
    expect(setup.captureCharFrame()).toContain("Rename note.txt")

    for (let i = 0; i < "note.txt".length; i++) setup.mockInput.pressBackspace()
    await setup.mockInput.typeText("renamed.txt")
    setup.mockInput.pressEnter()
    try {
      await waitFor(() => {
        const current = state()
        return current.phase === "ready" && "workspace" in current
          && current.workspace.left.entries.some((entry) => entry.name_display === "renamed.txt")
          && !current.workspace.left.entries.some((entry) => entry.name_display === "note.txt")
      }, 5_000)
    } catch (error) {
      const current = state()
      const debug = current.phase === "ready" && "session" in current
        ? JSON.stringify({ tasks: current.actionTasks, error: "commandError" in current ? current.commandError : undefined, entries: "workspace" in current ? current.workspace.left.entries.map((entry) => entry.name_display) : [] })
        : current.phase
      throw new Error(`rename did not land: ${debug}`, { cause: error })
    }
    expect(await Bun.file(resolve(left, "renamed.txt")).text()).toBe("note")

    // Renaming onto an existing name surfaces a failed task, not silent damage.
    const tasksBeforeConflict = state()
    const failedBefore = tasksBeforeConflict.phase === "ready" && "session" in tasksBeforeConflict
      ? (tasksBeforeConflict.actionTasks ?? []).filter((task) => task.state === "failed").length
      : -1
    // The rename refreshed the pane; park the cursor back on renamed.txt (it
    // sorts after conflict.txt) so the dialog targets the intended entry.
    setup.mockInput.pressKey("G")
    await waitFor(() => {
      const current = state()
      return current.phase === "ready" && "workspace" in current
        && current.workspace.left.entries[current.workspace.left.cursor]?.name_display === "renamed.txt"
    })
    setup.mockInput.pressKey("c")
    setup.mockInput.pressKey("w")
    await setup.renderOnce()
    expect(setup.captureCharFrame()).toContain("Rename renamed.txt")
    for (let i = 0; i < "renamed.txt".length; i++) setup.mockInput.pressBackspace()
    await setup.mockInput.typeText("conflict.txt")
    setup.mockInput.pressEnter()
    try {
      await waitFor(() => {
        const current = state()
        return current.phase === "ready" && "session" in current
          && (current.actionTasks ?? []).filter((task) => task.state === "failed").length > failedBefore
      }, 5_000)
    } catch (error) {
      const current = state()
      const debug = current.phase === "ready" && "session" in current
        ? JSON.stringify({ tasks: current.actionTasks, entries: "workspace" in current ? current.workspace.left.entries.map((entry) => entry.name_display) : [] })
        : current.phase
      throw new Error(`conflict rename did not fail as expected: ${debug}`, { cause: error })
    }
    expect(await Bun.file(resolve(left, "renamed.txt")).text()).toBe("note")
    expect(await Bun.file(resolve(left, "conflict.txt")).text()).toBe("conflict")

    // u undoes the successful rename; the conflict attempt stays untouched.
    setup.mockInput.pressKey("u")
    await waitFor(() => {
      const current = state()
      return current.phase === "ready" && "workspace" in current
        && current.workspace.left.entries.some((entry) => entry.name_display === "note.txt")
        && !current.workspace.left.entries.some((entry) => entry.name_display === "renamed.txt")
    })
    expect(await Bun.file(resolve(left, "note.txt")).text()).toBe("note")
    expect(errors).toEqual([])
  } finally {
    setup.renderer.destroy()
    session.close()
    await session.completion
  }
}, { timeout: 60000 })

test.skipIf(process.platform === "win32")("real session batch renames a selection through cw with Esc abort and per-entry undo", async () => {
  const executable = process.env.NEOVIFM_CORE_SESSION
  if (executable === undefined || executable.length === 0) {
    throw new Error("NEOVIFM_CORE_SESSION must point to the built core session")
  }
  root = await mkdtemp(resolve(tmpdir(), "neovifm-batch-rename-"))
  const left = resolve(root, "left")
  const right = resolve(root, "right")
  await mkdir(left)
  await mkdir(right)
  await writeFile(resolve(left, "one.txt"), "one")
  await writeFile(resolve(left, "three.txt"), "three")
  await writeFile(resolve(left, "two.txt"), "two")

  const [state, setState] = createSignal<ProbeState>(initialProbeState())
  const errors: Error[] = []
  const session = startCoreSession({
    executable,
    leftPath: left,
    rightPath: right,
    onRecord: (record) => setState((previous) => reduceProbeState(previous, record)),
    onError: (error) => errors.push(error),
  })
  const appProps = () => {
    const current = state()
    return {
      workspace: current.phase === "ready" && "workspace" in current ? current.workspace : undefined,
      capabilities: current.phase === "ready" ? current.hello.capabilities : undefined,
      onCommand: (command: Parameters<typeof session.send>[0]) => session.send(command),
    }
  }
  const setup = await testRender(() => <App {...appProps()} />, { width: 100, height: 20 })
  const names = () => {
    const current = state()
    if (!(current.phase === "ready" && "workspace" in current)) return []
    return current.workspace.left.entries.map((entry) => entry.name_display)
  }

  try {
    await waitFor(() => names().join(",") === "one.txt,three.txt,two.txt")

    // Select all three entries in order: one.txt (0), three.txt (1), two.txt (2).
    for (const _ of ["one.txt", "three.txt", "two.txt"]) {
      setup.mockInput.pressKey("t")
      setup.mockInput.pressKey("j")
    }
    await waitFor(() => {
      const current = state()
      return current.phase === "ready" && "workspace" in current && current.workspace.left.selection_count === 3
    })

    setup.mockInput.pressKey("c")
    setup.mockInput.pressKey("w")
    await setup.renderOnce()
    expect(setup.captureCharFrame()).toContain("Rename one.txt (1/3)")

    for (let i = 0; i < "one.txt".length; i++) setup.mockInput.pressBackspace()
    await setup.mockInput.typeText("r-one.txt")
    setup.mockInput.pressEnter()
    await waitFor(() => names().includes("r-one.txt"))
    await setup.renderOnce()
    expect(setup.captureCharFrame()).toContain("Rename three.txt (2/3)")

    for (let i = 0; i < "three.txt".length; i++) setup.mockInput.pressBackspace()
    await setup.mockInput.typeText("r-three.txt")
    setup.mockInput.pressEnter()
    await waitFor(() => names().includes("r-three.txt"))
    await setup.renderOnce()
    expect(setup.captureCharFrame()).toContain("Rename two.txt (3/3)")

    // Aborting abandons only the remaining queue entries. The escape-key flush
    // in opentui is delayed to disambiguate Alt prefixes, so let it settle
    // before the next key.
    setup.mockInput.pressEscape()
    await Bun.sleep(60)
    await setup.renderOnce()
    expect(setup.captureCharFrame()).not.toContain("Rename two.txt")
    await waitFor(() => names().includes("two.txt") && names().includes("r-one.txt") && names().includes("r-three.txt"))
    expect(await Bun.file(resolve(left, "r-one.txt")).text()).toBe("one")
    expect(await Bun.file(resolve(left, "r-three.txt")).text()).toBe("three")
    expect(await Bun.file(resolve(left, "two.txt")).text()).toBe("two")

    // Each queue item produced its own undo step; u walks them back one by one.
    setup.mockInput.pressKey("u")
    await waitFor(() => names().includes("three.txt") && !names().includes("r-three.txt"))
    setup.mockInput.pressKey("u")
    await waitFor(() => names().includes("one.txt") && !names().includes("r-one.txt"))
    expect(names().join(",")).toBe("one.txt,three.txt,two.txt")
    expect(errors).toEqual([])
  } finally {
    setup.renderer.destroy()
    session.close()
    await session.completion
  }
}, { timeout: 60000 })
