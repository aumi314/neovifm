import { afterEach, expect, test } from "bun:test"
import { mkdir, mkdtemp, readFile, rm, writeFile } from "node:fs/promises"
import { tmpdir } from "node:os"
import { resolve } from "node:path"

import { startCoreSession, type CoreSession, type CoreSessionCommand } from "../src/core-client.js"
import { openResolvedFile } from "../src/open-file.js"
import { initialProbeState, reduceProbeState, type ProbeState } from "../src/probe-state.js"

let root: string | undefined
let extensionKey: string | undefined
let programKey: string | undefined
const sessions = new Set<CoreSession>()

function runRegistry(args: readonly string[], allowFailure = false): void {
  const result = Bun.spawnSync({ cmd: ["reg.exe", ...args], stdout: "ignore", stderr: "pipe" })
  if (!allowFailure && result.exitCode !== 0) {
    throw new Error(`registry command failed: ${new TextDecoder().decode(result.stderr)}`)
  }
}

afterEach(async () => {
  for (const session of sessions) {
    session.close()
    await session.completion
  }
  sessions.clear()
  if (extensionKey !== undefined) runRegistry(["delete", extensionKey, "/f"], true)
  if (programKey !== undefined) runRegistry(["delete", programKey, "/f"], true)
  extensionKey = undefined
  programKey = undefined
  if (root !== undefined) await rm(root, { recursive: true, force: true })
  root = undefined
})

async function waitFor(predicate: () => boolean | Promise<boolean>, timeoutMs = 20_000, diagnostic?: () => unknown): Promise<void> {
  const deadline = Date.now() + timeoutMs
  while (!(await predicate())) {
    if (Date.now() >= deadline) throw new Error(`timed out waiting for Windows opener: ${JSON.stringify(diagnostic?.())}`)
    await Bun.sleep(10)
  }
}

function readyState(state: ProbeState): Extract<ProbeState, { readonly session: true }> {
  if (state.phase === "ready" && "session" in state) return state
  throw new Error("expected ready Windows opener session")
}

function openCommand(state: ProbeState, association?: readonly string[]): CoreSessionCommand {
  const current = readyState(state)
  const pane = current.workspace.left
  const entry = pane.entries[0]
  if (entry === undefined || pane.cwd_device === undefined || pane.cwd_inode === undefined
    || pane.cwd_ctime_unix_ns === undefined || entry.device === undefined
    || entry.inode === undefined || entry.ctime_unix_ns === undefined) {
    throw new Error("expected a Windows opener target with stable identity")
  }
  return {
    action: "open",
    intent: "open",
    pane: "left",
    cwd_bytes_hex: pane.cwd_bytes_hex,
    snapshot_revision: pane.snapshot_revision,
    cwd_device: pane.cwd_device,
    cwd_inode: pane.cwd_inode,
    cwd_ctime_unix_ns: pane.cwd_ctime_unix_ns,
    path_bytes_hex: entry.path_bytes_hex,
    device: entry.device,
    inode: entry.inode,
    ctime_unix_ns: entry.ctime_unix_ns,
    ...(association === undefined ? {} : { association_argv: association }),
  }
}

function findPowerShell(): string {
  const result = Bun.spawnSync({ cmd: ["where.exe", "pwsh.exe"], stdout: "pipe", stderr: "pipe" })
  const executable = new TextDecoder().decode(result.stdout).split(/\r?\n/u).find((line) => line.length > 0)
  if (result.exitCode !== 0 || executable === undefined) throw new Error("pwsh.exe is required for the Windows association fixture")
  return executable
}

function registryCommand(executable: string, script: string, capture: string): string {
  const quote = (value: string): string => `"${value.replaceAll('"', '\\"')}"`
  return [quote(executable), "-NoLogo", "-NoProfile", "-NonInteractive",
    "-WindowStyle", "Hidden", "-File", quote(script), quote(capture), '"%1"'].join(" ")
}

test.skipIf(process.platform !== "win32")("real Windows core opens a Unicode long path through the registered default application", async () => {
  const core = process.env.NEOVIFM_CORE_SESSION
  if (core === undefined || core.length === 0) throw new Error("NEOVIFM_CORE_SESSION must point to the built Windows core")
  root = await mkdtemp(resolve(tmpdir(), "neovifm-windows-opener-"))
  let left = root
  let index = 0
  while (left.length < 280) {
    left = resolve(left, `层-${String(index++).padStart(2, "0")}-abcdefghijklmnop`)
    await mkdir(left)
  }
  const right = resolve(root, "right")
  await mkdir(right)
  const suffix = `${process.pid}-${Date.now()}`
  const extension = `.nvo${suffix}`
  const program = `NeoVifm.OpenTest.${suffix}`
  const target = resolve(left, `打开 me-中文${extension}`)
  const capture = resolve(root, "captured-path.txt")
  const captureScript = resolve(root, "capture-open.ps1")
  await writeFile(target, "open me")
  await writeFile(captureScript,
    "param([string]$Output, [string]$Target)\n"
    + "[IO.File]::WriteAllText($Output, $Target, [Text.UTF8Encoding]::new($false))\n")

  extensionKey = `HKCU\\Software\\Classes\\${extension}`
  programKey = `HKCU\\Software\\Classes\\${program}`
  runRegistry(["add", extensionKey, "/ve", "/d", program, "/f"])
  runRegistry(["add", `${programKey}\\shell\\open\\command`, "/ve", "/d",
    registryCommand(findPowerShell(), captureScript, capture), "/f"])

  let state: ProbeState = initialProbeState()
  const errors: Error[] = []
  const session = startCoreSession({
    executable: core,
    leftPath: left,
    rightPath: right,
    onRecord: (record) => { state = reduceProbeState(state, record) },
    onError: (error) => errors.push(error),
  })
  sessions.add(session)
  await waitFor(() => state.phase === "ready" && "session" in state)

  let sequence = readyState(state).commandSequence + 1
  expect(await session.send(openCommand(state))).toBe(true)
  await waitFor(() => readyState(state).open?.command_sequence === sequence, 20_000,
    () => ({ state, errors: errors.map(String) }))
  let opened = readyState(state).open
  expect(opened).toMatchObject({ source: "platform", state: "resolved" })
  const helper = opened?.argv[0]
  if (helper === undefined) throw new Error("expected Windows platform opener argv")
  const openedTarget = opened?.argv[1]
  if (openedTarget === undefined) throw new Error("expected Windows platform opener target")
  expect(helper).toEndWith("neovifm-win-open.exe")
  expect(resolve(openedTarget)).toBe(target)
  expect(resolve(helper)).toBe(helper)

  await openResolvedFile(opened!.argv)
  await waitFor(async () => {
    try {
      return resolve(await readFile(capture, "utf8")) === target
    } catch {
      return false
    }
  })
  expect(resolve(await readFile(capture, "utf8"))).toBe(target)

  sequence = readyState(state).commandSequence + 1
  expect(await session.send(openCommand(state, [process.execPath, captureScript]))).toBe(true)
  await waitFor(() => readyState(state).open?.command_sequence === sequence)
  opened = readyState(state).open
  expect(opened).toMatchObject({ source: "association" })
  expect(opened!.argv.slice(0, 2)).toEqual([process.execPath, captureScript])
  expect(errors).toEqual([])
}, { timeout: 60_000 })

test.skipIf(process.platform !== "win32")("Windows opener helper rejects invalid and missing targets with diagnostics", async () => {
  const core = process.env.NEOVIFM_CORE_SESSION
  if (core === undefined || core.length === 0) throw new Error("NEOVIFM_CORE_SESSION must point to the built Windows core")
  const helper = resolve(core, "..", "neovifm-win-open.exe")
  const invalid = Bun.spawn({ cmd: [helper], stdin: "ignore", stdout: "ignore", stderr: "pipe" })
  const [invalidExit, invalidStderr] = await Promise.all([invalid.exited, new Response(invalid.stderr).text()])
  expect(invalidExit).toBe(2)
  expect(invalidStderr).toContain("expected one non-empty target path")

  await expect(openResolvedFile([helper, resolve(tmpdir(), "missing-neovifm-open-target.nvo")]))
    .rejects.toThrow("neovifm-win-open: system opener failed")
}, { timeout: 20_000 })
