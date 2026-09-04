import { afterEach, expect, test } from "bun:test"
import { chmod, mkdtemp, mkdir, rm, writeFile } from "node:fs/promises"
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
    if (Date.now() >= deadline) throw new Error("timed out waiting for yank/put session update")
    await Bun.sleep(10)
  }
}

// Windows action identity uses a dedicated fixture suite; see windows-actions.test.ts.
test.skipIf(process.platform === "win32")("real session yanks, puts, and undoes through Vifm register keys", async () => {
  const executable = process.env.NEOVIFM_CORE_SESSION
  if (executable === undefined || executable.length === 0) {
    throw new Error("NEOVIFM_CORE_SESSION must point to the built core session")
  }
  root = await mkdtemp(resolve(tmpdir(), "neovifm-yank-put-"))
  const left = resolve(root, "left")
  const right = resolve(root, "right")
  const trashHelper = resolve(root, "test-trash.sh")
  await mkdir(left)
  await mkdir(right)
  await mkdir(resolve(left, "a-dir"))
  await writeFile(trashHelper, "#!/bin/sh\nexec /bin/rm -rf -- \"$1\"\n")
  await chmod(trashHelper, 0o700)
  await writeFile(resolve(left, "note.txt"), "note")

  const originalTrash = process.env.NEOVIFM_TRASH_EXECUTABLE
  process.env.NEOVIFM_TRASH_EXECUTABLE = trashHelper
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
  const doneActionCount = () => {
    const current = state()
    return current.phase === "ready" && "session" in current
      ? (current.actionTasks ?? []).filter((task) => task.state === "done").length
      : -1
  }
  const setup = await testRender(() => <App {...appProps()} />, { width: 100, height: 20 })

  try {
    await waitFor(() => {
      const current = state()
      return current.phase === "ready" && "workspace" in current
        && current.workspace.left.entries.some((entry) => entry.name_display === "note.txt")
    })
    await setup.renderOnce()

    // Name-sorted entries put a-dir before note.txt; move onto note.txt first.
    setup.mockInput.pressKey("j")
    await waitFor(() => {
      const current = state()
      return current.phase === "ready" && "workspace" in current
        && current.workspace.left.entries[current.workspace.left.cursor]?.name_display === "note.txt"
    })

    // Yank is client-local: no command and no action task may reach the core.
    const actionsBeforeYank = doneActionCount()
    setup.mockInput.pressKey("y")
    setup.mockInput.pressKey("y")
    await setup.renderOnce()
    expect(setup.captureCharFrame()).toContain("1 item(s) yanked")
    expect(doneActionCount()).toBe(actionsBeforeYank)

    // p puts a copy into the opposite pane.
    setup.mockInput.pressTab()
    await waitFor(() => {
      const current = state()
      return current.phase === "ready" && "workspace" in current && current.workspace.active_pane === "right"
    })
    setup.mockInput.pressKey("p")
    await waitFor(() => {
      const current = state()
      return current.phase === "ready" && "workspace" in current
        && current.workspace.right.entries.some((entry) => entry.name_display === "note.txt")
        && current.workspace.left.entries.some((entry) => entry.name_display === "note.txt")
    })
    expect(await Bun.file(resolve(right, "note.txt")).text()).toBe("note")

    // u undoes the put through the core-owned bridge.
    setup.mockInput.pressKey("u")
    await waitFor(() => {
      const current = state()
      return current.phase === "ready" && "workspace" in current
        && !current.workspace.right.entries.some((entry) => entry.name_display === "note.txt")
    })

    // P puts by moving; the source disappears from the left pane.
    setup.mockInput.pressTab()
    await waitFor(() => {
      const current = state()
      return current.phase === "ready" && "workspace" in current && current.workspace.active_pane === "left"
    })
    // The undo may have moved the cursor; park it back on note.txt before yanking.
    setup.mockInput.pressKey("g")
    setup.mockInput.pressKey("g")
    await waitFor(() => {
      const current = state()
      return current.phase === "ready" && "workspace" in current && current.workspace.left.cursor === 0
    })
    setup.mockInput.pressKey("j")
    await waitFor(() => {
      const current = state()
      return current.phase === "ready" && "workspace" in current
        && current.workspace.left.entries[current.workspace.left.cursor]?.name_display === "note.txt"
    })
    setup.mockInput.pressKey("y")
    setup.mockInput.pressKey("y")
    await setup.renderOnce()
    setup.mockInput.pressTab()
    await waitFor(() => {
      const current = state()
      return current.phase === "ready" && "workspace" in current && current.workspace.active_pane === "right"
    })
    setup.mockInput.pressKey("P")
    await waitFor(() => {
      const current = state()
      return current.phase === "ready" && "workspace" in current
        && current.workspace.right.entries.some((entry) => entry.name_display === "note.txt")
        && !current.workspace.left.entries.some((entry) => entry.name_display === "note.txt")
    })
    expect(await Bun.file(resolve(right, "note.txt")).text()).toBe("note")

    // Yank again, then make the source leave both panes; put must be refused client-side.
    setup.mockInput.pressTab()
    await waitFor(() => {
      const current = state()
      return current.phase === "ready" && "workspace" in current && current.workspace.active_pane === "left"
    })
    setup.mockInput.pressKey("y")
    setup.mockInput.pressKey("y")
    await setup.renderOnce()
    // The cursor sits on a-dir after the move; enter it so /left is no longer shown.
    setup.mockInput.pressKey("g")
    setup.mockInput.pressKey("g")
    await waitFor(() => {
      const current = state()
      return current.phase === "ready" && "workspace" in current && current.workspace.left.cursor === 0
    })
    setup.mockInput.pressKey("l")
    await waitFor(() => {
      const current = state()
      return current.phase === "ready" && "workspace" in current && current.workspace.left.cwd_display.endsWith("/a-dir")
    })
    const actionsBeforeRefusedPut = doneActionCount()
    setup.mockInput.pressKey("p")
    await setup.renderOnce()
    // The status bar truncates the tail at 100 columns; assert the stable prefix.
    expect(setup.captureCharFrame()).toContain("Yank source directory is no longer")
    expect(doneActionCount()).toBe(actionsBeforeRefusedPut)

    expect(errors).toEqual([])
  } finally {
    setup.renderer.destroy()
    session.close()
    await session.completion
    if (originalTrash === undefined) delete process.env.NEOVIFM_TRASH_EXECUTABLE
    else process.env.NEOVIFM_TRASH_EXECUTABLE = originalTrash
  }
}, { timeout: 60000 })
