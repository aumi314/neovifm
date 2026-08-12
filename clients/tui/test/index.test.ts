import { afterEach, expect, test } from "bun:test"

import { CoreClientError, type CoreSession, type CoreSessionRequest } from "../src/core-client.js"
import {
  appPropsFor,
  cliHelp,
  cliVersion,
  checkCore,
  defaultCoreProbePath,
  parseCliArgs,
  resolveCoreSessionPath,
  copyText,
  editorCommand,
  exitCodeFor,
  isStandaloneRuntime,
  main,
  openEditor,
  openFile,
  openResolvedFile,
  renderUntilDestroyed,
  runCli,
  type MainDependencies,
  toUiErrorMessage,
} from "../src/index.js"
import type { OpenProcess, OpenSpawnOptions } from "../src/open-file.js"
import { initialProbeState, reduceProbeState } from "../src/probe-state.js"
import { parseProtocolRecord } from "../src/protocol.js"

const hello = parseProtocolRecord({
  protocol: "neovifm-core",
  version: 0,
  type: "hello",
  sequence: 0,
  payload: { implementation: "probe", capabilities: ["snapshot-v0"] },
})

const snapshot = parseProtocolRecord({
  protocol: "neovifm-core",
  version: 0,
  type: "snapshot",
  sequence: 1,
  payload: {
    cwd_display: "/tmp",
    cwd_bytes_hex: "2f746d70",
    generated_at_unix_ms: "0",
    cursor: -1,
    entry_count: 0,
    entries: [],
  },
})

const workspaceHello = parseProtocolRecord({
  protocol: "neovifm-core",
  version: 1,
  type: "hello",
  sequence: 0,
  payload: { implementation: "workspace", capabilities: ["workspace-v1"] },
})

const workspace = parseProtocolRecord({
  protocol: "neovifm-core",
  version: 1,
  type: "workspace-snapshot",
  sequence: 1,
  payload: {
    active_pane: "left",
    left: snapshot.type === "snapshot" ? snapshot.payload : {},
    right: snapshot.type === "snapshot" ? { ...snapshot.payload, cwd_display: "/var" } : {},
  },
})

afterEach(() => {
  process.exitCode = 0
})

function dependencies(start: (request: CoreSessionRequest) => CoreSession): MainDependencies {
  return {
    defaultCoreProbePath: () => "/mock/neovifm-core-probe",
    renderApp: async (props) => {
      props()
    },
    startCoreSession: start,
  }
}

test("derives loading, workspace-ready, and core-error app props from immutable state", () => {
  const waiting = reduceProbeState(initialProbeState(), workspaceHello)
  const ready = reduceProbeState(waiting, workspace)
  const failed = reduceProbeState(waiting, parseProtocolRecord({
    protocol: "neovifm-core",
    version: 1,
    type: "error",
    sequence: 1,
    payload: { code: "denied", message: "permission denied", retryable: false },
  }))

  expect(appPropsFor(waiting)).toMatchObject({ loading: true })
  expect(appPropsFor(ready)).toMatchObject({
    loading: false,
    workspace: workspace.type === "workspace-snapshot" ? workspace.payload : {},
    capabilities: ["workspace-v1"],
  })
  expect(appPropsFor(failed)).toMatchObject({ error: "permission denied" })
  expect(appPropsFor(ready, "client failed")).toMatchObject({ error: "client failed" })
})

test("uses a stable default location and sanitizes unknown errors", () => {
  expect(defaultCoreProbePath()).toEndWith(process.platform === "win32"
    ? "neovifm-core-session.exe"
    : "neovifm-core-session")
  expect(toUiErrorMessage(new Error("bad\u001bmessage"))).toBe("bad�message")
})

const sessionHello = parseProtocolRecord({
  protocol: "neovifm-core",
  version: 3,
  type: "hello",
  sequence: 0,
  payload: { implementation: "session", capabilities: ["preview-session-v3"] },
})

const sessionWorkspace = parseProtocolRecord({
  protocol: "neovifm-core",
  version: 3,
  type: "workspace-snapshot",
  sequence: 1,
  payload: {
    command_sequence: 0,
    trigger: "initial",
    active_pane: "left",
    left: snapshot.type === "snapshot" ? snapshot.payload : {},
    right: snapshot.type === "snapshot" ? snapshot.payload : {},
  },
})

test("parses the experimental portable CLI without treating paths as options", () => {
  expect(parseCliArgs([])).toEqual({ action: "run", paths: [] })
  expect(parseCliArgs(["left", "right"])).toEqual({ action: "run", paths: ["left", "right"] })
  expect(parseCliArgs(["--", "-left"])).toEqual({ action: "run", paths: ["-left"] })
  expect(parseCliArgs(["--help"])).toEqual({ action: "help" })
  expect(parseCliArgs(["--version"])).toEqual({ action: "version" })
  expect(parseCliArgs(["--check"])).toEqual({ action: "check" })
  expect(() => parseCliArgs(["--unknown"])).toThrow("Unknown option")
  expect(() => parseCliArgs(["one", "two", "three"])).toThrow("at most two")
})

test("resolves core overrides before standalone and source-tree defaults", () => {
  const platform = process.platform
  const executable = platform === "win32" ? "neovifm-core-session.exe" : "neovifm-core-session"
  expect(resolveCoreSessionPath({
    environment: { NEOVIFM_CORE_SESSION: " /new/core ", NEOVIFM_CORE_PROBE: "/legacy/core" },
    standalone: true,
    execPath: `/portable/neovifm${platform === "win32" ? ".exe" : ""}`,
    sourceDirectory: "/source/clients/tui/src",
    platform,
  })).toBe("/new/core")
  expect(resolveCoreSessionPath({
    environment: { NEOVIFM_CORE_PROBE: "/legacy/core" },
    standalone: true,
    execPath: `/portable/neovifm${platform === "win32" ? ".exe" : ""}`,
    sourceDirectory: "/source/clients/tui/src",
    platform,
  })).toBe("/legacy/core")
  expect(resolveCoreSessionPath({
    environment: {}, standalone: true, execPath: "/portable/neovifm",
    sourceDirectory: "/source/clients/tui/src", platform: "linux",
  })).toBe(`/portable/${executable.replace(".exe", "")}`)
  expect(resolveCoreSessionPath({
    environment: {}, standalone: false, execPath: "/portable/neovifm",
    sourceDirectory: "/source/clients/tui/src", platform: "linux",
  })).toBe("/source/src/neovifm-core-session")
})

test("detects Bun 1.3 standalone executables by their runtime filename", () => {
  expect(isStandaloneRuntime("/usr/local/bin/bun", "linux")).toBe(false)
  expect(isStandaloneRuntime("C:\\tools\\bun.exe", "win32")).toBe(false)
  expect(isStandaloneRuntime("/opt/neovifm/neovifm", "linux")).toBe(true)
  expect(isStandaloneRuntime("C:\\NeoVifm\\neovifm.exe", "win32")).toBe(true)
})

test("publishes deterministic help and build metadata", () => {
  expect(cliHelp()).toContain("neovifm [LEFT [RIGHT]]")
  expect(cliHelp()).toContain("--check")
  expect(cliVersion({ commit: "1234567890abcdef", platform: "linux", arch: "x64" }))
    .toBe("NeoVifm Workbench Alpha 0 (unreleased) 1234567890ab linux-x64")
})

test("checks a real v3 session without restoring or persisting workspace state", async () => {
  let request: CoreSessionRequest | undefined
  let closed = false
  const result = await checkCore("/portable/neovifm-core-session", "/路径 with spaces", {
    timeoutMs: 100,
    startCoreSession: (value) => {
      request = value
      value.onRecord(sessionHello)
      value.onRecord(sessionWorkspace)
      return {
        completion: Promise.resolve(),
        send: async () => false,
        close: () => { closed = true },
      }
    },
  })

  expect(request).toMatchObject({
    executable: "/portable/neovifm-core-session",
    leftPath: "/路径 with spaces",
    rightPath: "/路径 with spaces",
    resume: false,
    persist: false,
  })
  expect(result).toEqual({ protocolVersion: 3, implementation: "session", capabilities: ["preview-session-v3"] })
  expect(closed).toBe(true)
})

test("fails package checks that never publish an initial v3 workspace", async () => {
  await expect(checkCore("/missing/core", "/tmp", {
    timeoutMs: 10,
    startCoreSession: () => ({
      completion: new Promise<void>(() => undefined),
      send: async () => false,
      close: () => undefined,
    }),
  })).rejects.toThrow("timed out")
})

test("rejects invalid check timeouts and synchronous core spawn failures", async () => {
  await expect(checkCore("/core", "/tmp", {
    timeoutMs: 0,
    startCoreSession: () => { throw new Error("must not start") },
  })).rejects.toThrow("positive safe integer")
  await expect(checkCore("/missing/core", "/tmp", {
    timeoutMs: 100,
    startCoreSession: () => { throw new CoreClientError("missing core", { kind: "spawn" }) },
  })).rejects.toMatchObject({ kind: "spawn" })
})

test("rejects a structured core error during package checks", async () => {
  const coreError = parseProtocolRecord({
    protocol: "neovifm-core",
    version: 3,
    type: "error",
    sequence: 1,
    payload: { code: "open-directory", message: "denied", retryable: false },
  })
  await expect(checkCore("/core", "/tmp", {
    timeoutMs: 100,
    startCoreSession: (request) => {
      request.onRecord(sessionHello)
      request.onRecord(coreError)
      return { completion: Promise.resolve(), send: async () => false, close: () => undefined }
    },
  })).rejects.toMatchObject({ kind: "core", coreCode: "open-directory" })
})

test("propagates package check callbacks and invalid protocol records", async () => {
  await expect(checkCore("/core", "/tmp", {
    timeoutMs: 100,
    startCoreSession: (request) => {
      request.onError(new CoreClientError("callback failure", { kind: "protocol" }))
      return { completion: Promise.resolve(), send: async () => false, close: () => undefined }
    },
  })).rejects.toMatchObject({ kind: "protocol" })

  await expect(checkCore("/core", "/tmp", {
    timeoutMs: 100,
    startCoreSession: (request) => {
      request.onRecord(sessionWorkspace)
      return { completion: Promise.resolve(), send: async () => false, close: () => undefined }
    },
  })).rejects.toThrow("Expected hello")

  await expect(checkCore("/core", "/tmp", {
    timeoutMs: 100,
    startCoreSession: () => ({
      completion: Promise.reject(new Error("completion failure")),
      send: async () => false,
      close: () => undefined,
    }),
  })).rejects.toThrow("completion failure")
})

test("runs help, check, and usage errors without mounting the full-screen renderer", async () => {
  const stdout: string[] = []
  const stderr: string[] = []
  let rendered = false
  const base = dependencies((request) => {
    request.onRecord(sessionHello)
    request.onRecord(sessionWorkspace)
    return { completion: Promise.resolve(), send: async () => false, close: () => undefined }
  })
  const runtime = {
    stdout: (line: string) => { stdout.push(line) },
    stderr: (line: string) => { stderr.push(line) },
    cwd: () => "/portable cwd",
  }

  expect(await runCli(["--help"], { ...base, renderApp: async () => { rendered = true } }, runtime)).toBe(0)
  expect(await runCli(["--check"], base, runtime)).toBe(0)
  expect(await runCli(["--bad"], base, runtime)).toBe(2)
  expect(rendered).toBe(false)
  expect(stdout.join("\n")).toContain("Usage: neovifm")
  expect(stdout.join("\n")).toContain("protocol v3")
  expect(stderr.join("\n")).toContain("Unknown option")
})

test("runs version, normal workspace, and failed check CLI paths", async () => {
  const stdout: string[] = []
  const stderr: string[] = []
  let rendered = false
  const runtime = {
    stdout: (line: string) => { stdout.push(line) },
    stderr: (line: string) => { stderr.push(line) },
    cwd: () => "/tmp",
  }
  const ready = dependencies((request) => {
    request.onRecord(sessionHello)
    request.onRecord(sessionWorkspace)
    return { completion: Promise.resolve(), send: async () => false, close: () => undefined }
  })
  expect(await runCli(["--version"], ready, runtime)).toBe(0)
  expect(await runCli(["/tmp"], {
    ...ready,
    renderApp: async () => { rendered = true },
  }, runtime)).toBe(0)
  expect(rendered).toBe(true)

  const failed = dependencies(() => { throw new CoreClientError("missing", { kind: "spawn" }) })
  expect(await runCli(["--check"], failed, runtime)).toBe(1)
  expect(stdout.join("\n")).toContain("Workbench Alpha 0")
  expect(stderr.join("\n")).toContain("package check failed")
})

test("builds a direct editor argv without invoking a shell", () => {
  expect(editorCommand("/tmp/file name.ts", { VISUAL: "code --wait" })).toEqual([
    "code", "--wait", "--", "/tmp/file name.ts",
  ])
  expect(editorCommand("/tmp/file", { EDITOR: "'nvim' -f" })).toEqual([
    "nvim", "-f", "--", "/tmp/file",
  ])
})

test("opens a file through a structured platform argv without invoking a shell", async () => {
  let options: OpenSpawnOptions | undefined
  const process: OpenProcess = { exited: Promise.resolve(0) }
  await openFile("/tmp/file name.pdf", {
    platform: "darwin",
    spawn: (value) => {
      options = value
      return process
    },
  })
  expect(options).toEqual({
    cmd: ["/usr/bin/open", "/tmp/file name.pdf"],
    stdin: "ignore",
    stdout: "ignore",
    stderr: "pipe",
  })
})

test("rejects invalid editor, resolved-open, and clipboard input before spawning", async () => {
  await expect(openEditor("")).rejects.toThrow("Editor path is invalid")
  await expect(openResolvedFile([])).rejects.toThrow("Open command is empty")
  await expect(copyText("x".repeat(1024 * 1024 + 1))).rejects.toThrow("limit is 1048576 bytes")
})

test("preserves structured core error context without rendering stderr", () => {
  const error = new CoreClientError("permission denied", {
    kind: "core",
    coreCode: "open-directory",
    exitCode: 2,
    stderr: "diagnostic that must stay out of the UI",
    stderrTruncated: true,
  })

  expect(toUiErrorMessage(error)).toContain("core")
  expect(toUiErrorMessage(error)).toContain("open-directory")
  expect(toUiErrorMessage(error)).not.toContain("that must stay out of the UI")
  expect(toUiErrorMessage(error)).toContain("diagnostics truncated")
  expect(exitCodeFor(error)).toBe(2)
})

test("maps cancellation to a conventional non-zero CLI status", () => {
  const error = new CoreClientError("cancelled", { kind: "cancelled" })

  expect(exitCodeFor(error)).toBe(130)
})

test("starts the session before rendering and applies records through the reducer", async () => {
  const calls: string[] = []

  await main(["/tmp"], dependencies((request) => {
    request.onRecord(workspaceHello)
    request.onRecord(workspace)
    calls.push(request.executable, request.leftPath, request.rightPath)
    return { completion: Promise.resolve(), send: async () => true, close: () => undefined }
  }))

  expect(calls).toEqual(["/mock/neovifm-core-probe", "/tmp", "/tmp"])
})

test("requests persisted session restore only when no paths are supplied", async () => {
  let resume: boolean | undefined
  let persist: boolean | undefined
  const start = (request: CoreSessionRequest): CoreSession => {
    resume = request.resume
    persist = request.persist
    request.onRecord(workspaceHello)
    request.onRecord(workspace)
    return { completion: Promise.resolve(), send: async () => true, close: () => undefined }
  }
  await main([], dependencies(start))
  expect(resume).toBe(true)
  expect(persist).toBe(true)
  await main(["/tmp"], dependencies(start))
  expect(resume).toBe(false)
  expect(persist).toBe(true)
})

test("injects the clipboard service into the rendered app", async () => {
  const copied: string[] = []
  const base = dependencies((request) => {
    request.onRecord(workspaceHello)
    request.onRecord(workspace)
    return { completion: Promise.resolve(), send: async () => true, close: () => undefined }
  })
  await main(["/tmp"], {
    ...base,
    copyText: async (text) => { copied.push(text) },
    renderApp: async (props) => {
      await props().onCopyText?.("~/project")
    },
  })

  expect(copied).toEqual(["~/project"])
})

test("keeps the production renderer lifecycle open until onDestroy", async () => {
  let destroy: (() => void) | undefined
  let finished = false
  const lifetime = renderUntilDestroyed(() => ({}), async (_node, config) => {
    destroy = config.onDestroy
  }).then(() => { finished = true })

  await Bun.sleep(0)
  expect(finished).toBe(false)
  expect(destroy).toBeDefined()
  destroy?.()
  await lifetime
  expect(finished).toBe(true)
})

test("keeps a non-zero exit status after displaying a structured core failure", async () => {
  await main([], dependencies((request) => {
    request.onError(new CoreClientError("permission denied", {
      kind: "core",
      coreCode: "open-directory",
      exitCode: 2,
    }))
    return { completion: Promise.resolve(), send: async () => true, close: () => undefined }
  }))

  expect(process.exitCode).toBe(2)
})

test("aborts and waits for the core probe when renderer startup fails", async () => {
  let startProbe!: () => void
  let aborted = false
  const probeStarted = new Promise<void>((resolve) => {
    startProbe = resolve
  })
  const failingDependencies: MainDependencies = {
    defaultCoreProbePath: () => "/mock/neovifm-core-probe",
    renderApp: async () => {
      await probeStarted
      throw new Error("renderer setup failed")
    },
    startCoreSession: (request) => {
      startProbe()
      const completion = new Promise<void>((resolve) => {
        request.signal?.addEventListener("abort", () => {
          aborted = true
          resolve()
        }, { once: true })
      })
      return { completion, send: async () => true, close: () => undefined }
    },
  }

  await expect(main([], failingDependencies)).rejects.toThrow("renderer setup failed")
  expect(aborted).toBe(true)
})
