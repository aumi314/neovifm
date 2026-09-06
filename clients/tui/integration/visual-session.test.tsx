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

async function waitFor(predicate: () => boolean, timeoutMs = 5_000): Promise<void> {
  const deadline = Date.now() + timeoutMs
  while (!predicate()) {
    if (Date.now() >= deadline) throw new Error("timed out waiting for visual session update")
    await Bun.sleep(10)
  }
}

test.skipIf(process.platform === "win32")("real session visual-line extends, shrinks, and survives exit; Ctrl-A and Escape manage the set", async () => {
  const executable = process.env.NEOVIFM_CORE_SESSION
  if (executable === undefined || executable.length === 0) {
    throw new Error("NEOVIFM_CORE_SESSION must point to the built core session")
  }
  root = await mkdtemp(resolve(tmpdir(), "neovifm-visual-"))
  const left = resolve(root, "left")
  const right = resolve(root, "right")
  await mkdir(left)
  await mkdir(right)
  await writeFile(resolve(left, "a"), "a")
  await writeFile(resolve(left, "b"), "b")
  await writeFile(resolve(left, "c"), "c")
  await writeFile(resolve(left, "d"), "d")

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
  const pane = () => {
    const current = state()
    if (!(current.phase === "ready" && "workspace" in current)) throw new Error("workspace disappeared")
    return current.workspace.left
  }

  try {
    await waitFor(() => {
      const current = state()
      return current.phase === "ready" && "workspace" in current && current.workspace.left.entry_count === 4
    })

    // Enter visual-line at the first entry: the anchor row is selected.
    setup.mockInput.pressKey("v")
    await waitFor(() => pane().selection_count === 1 && pane().entries[0]?.selected === true)

    // Extend twice, then shrink once: the boundary row is deselected again.
    setup.mockInput.pressKey("j")
    await waitFor(() => pane().selection_count === 2 && pane().cursor === 1)
    setup.mockInput.pressKey("j")
    await waitFor(() => pane().selection_count === 3 && pane().cursor === 2)
    setup.mockInput.pressKey("k")
    await waitFor(() => pane().selection_count === 2 && pane().cursor === 1)
    expect(pane().entries.filter((entry) => entry.selected).map((entry) => entry.name_display)).toEqual(["a", "b"])

    // Leaving visual mode keeps the selection set intact.
    setup.mockInput.pressEscape()
    await Bun.sleep(60)
    await waitFor(() => pane().selection_count === 2 && pane().entries.filter((entry) => entry.selected).length === 2)

    // Ctrl-A selects everything; a normal-mode Escape clears the set.
    setup.mockInput.pressKey("a", { ctrl: true })
    await waitFor(() => pane().selection_count === 4)
    setup.mockInput.pressEscape()
    await waitFor(() => pane().selection_count === 0)

    expect(errors).toEqual([])
  } finally {
    setup.renderer.destroy()
    session.close()
    await session.completion
  }
}, { timeout: 60000 })
