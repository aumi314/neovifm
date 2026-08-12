import { posix, resolve, win32 } from "node:path"
import { createSignal } from "solid-js"
import { render } from "@opentui/solid"
import type { CliRendererConfig } from "@opentui/core"

import { App, type AppProps } from "./app.js"
import { CoreClientError, startCoreSession } from "./core-client.js"
import { initialProbeState, reduceProbeState, type ProbeState } from "./probe-state.js"
import { sanitizeDisplayText } from "./protocol.js"
import { copyTextToClipboard } from "./clipboard.js"
import { openFile as launchOpenFile, openResolvedFile as launchResolvedOpenFile } from "./open-file.js"
import type { OpenFileDependencies } from "./open-file.js"

declare const __NEOVIFM_BUILD_COMMIT__: string

const WORKBENCH_STAGE = "Workbench Alpha 0 (unreleased)"

export type CliArguments =
  | Readonly<{ action: "run"; paths: readonly string[] }>
  | Readonly<{ action: "help" }>
  | Readonly<{ action: "version" }>
  | Readonly<{ action: "check" }>

export interface CorePathOptions {
  readonly environment: Readonly<Record<string, string | undefined>>
  readonly standalone: boolean
  readonly execPath: string
  readonly sourceDirectory: string
  readonly platform: NodeJS.Platform
}

export function parseCliArgs(args: readonly string[]): CliArguments {
  if (args.length === 1) {
    if (args[0] === "--help" || args[0] === "-h") return { action: "help" }
    if (args[0] === "--version" || args[0] === "-V") return { action: "version" }
    if (args[0] === "--check") return { action: "check" }
  }
  const separator = args.indexOf("--")
  const optionArguments = separator === -1 ? args : args.slice(0, separator)
  const paths = separator === -1 ? [...args] : [...optionArguments, ...args.slice(separator + 1)]
  const unknown = optionArguments.find((argument) => argument.startsWith("-"))
  if (unknown !== undefined) throw new Error(`Unknown option: ${unknown}`)
  if (paths.length > 2) throw new Error("NeoVifm accepts at most two directory paths")
  return { action: "run", paths }
}

export function cliHelp(): string {
  return [
    `NeoVifm ${WORKBENCH_STAGE}`,
    "Usage: neovifm [LEFT [RIGHT]]",
    "       neovifm --check",
    "",
    "Options:",
    "  -h, --help     Show this help",
    "  -V, --version  Show build information",
    "      --check    Verify the bundled core without starting the TUI",
    "      --         Treat remaining arguments as paths",
  ].join("\n")
}

export function cliVersion(options: Readonly<{
  commit?: string
  platform?: NodeJS.Platform
  arch?: string
}> = {}): string {
  const commit = (options.commit ?? (typeof __NEOVIFM_BUILD_COMMIT__ === "string"
    ? __NEOVIFM_BUILD_COMMIT__
    : "development")).slice(0, 12)
  return `NeoVifm ${WORKBENCH_STAGE} ${commit} ${options.platform ?? process.platform}-${options.arch ?? process.arch}`
}

export function resolveCoreSessionPath(options: CorePathOptions): string {
  const configured = options.environment.NEOVIFM_CORE_SESSION?.trim()
    || options.environment.NEOVIFM_CORE_PROBE?.trim()
  if (configured) return configured
  const path = options.platform === "win32" ? win32 : posix
  const executable = options.platform === "win32"
    ? "neovifm-core-session.exe"
    : "neovifm-core-session"
  return options.standalone
    ? path.resolve(path.dirname(options.execPath), executable)
    : path.resolve(options.sourceDirectory, "../../../src", executable)
}

export function isStandaloneRuntime(
  execPath: string = process.execPath,
  platform: NodeJS.Platform = process.platform,
): boolean {
  const path = platform === "win32" ? win32 : posix
  return !/^bun(?:\.exe)?$/i.test(path.basename(execPath))
}

export function defaultCoreProbePath(): string {
  const declaredStandalone = (Bun as unknown as { readonly isStandaloneExecutable?: boolean })
    .isStandaloneExecutable
  return resolveCoreSessionPath({
    environment: process.env,
    standalone: declaredStandalone ?? isStandaloneRuntime(),
    execPath: process.execPath,
    sourceDirectory: import.meta.dir,
    platform: process.platform,
  })
}

export interface MainDependencies {
  readonly defaultCoreProbePath: () => string
  readonly renderApp: (props: () => AppProps) => Promise<void>
  readonly startCoreSession: typeof startCoreSession
  readonly openEditor?: (path: string) => Promise<void>
  readonly openFile?: (path: string) => Promise<void>
  readonly openResolved?: (argv: readonly string[]) => Promise<void>
  readonly copyText?: (text: string) => Promise<void> | void
}

export interface CheckDependencies {
  readonly startCoreSession: typeof startCoreSession
  readonly timeoutMs?: number
}

export interface CoreCheckResult {
  readonly protocolVersion: 3
  readonly implementation: string
  readonly capabilities: readonly string[]
}

export interface CliRuntime {
  readonly stdout: (line: string) => void
  readonly stderr: (line: string) => void
  readonly cwd: () => string
}

export type RenderMount = (
  node: Parameters<typeof render>[0],
  config: CliRendererConfig,
) => Promise<void>

export function renderUntilDestroyed(
  props: () => AppProps,
  mount: RenderMount = render,
): Promise<void> {
  return new Promise<void>((resolve, reject) => {
    void mount(() => <App {...props()} />, { onDestroy: resolve }).catch(reject)
  })
}

const DEFAULT_MAIN_DEPENDENCIES: MainDependencies = {
  defaultCoreProbePath,
  renderApp: renderUntilDestroyed,
  startCoreSession,
  copyText,
}

function splitEditorCommand(command: string): readonly string[] {
  const parts = command.match(/(?:[^\s"']+|"[^"]*"|'[^']*')+/g) ?? []
  return parts.map((part) => {
    const quote = part[0]
    return (quote === '"' || quote === "'") && part.at(-1) === quote
      ? part.slice(1, -1)
      : part
  }).filter((part) => part.length !== 0)
}

export function editorCommand(
  path: string,
  environment: Readonly<Record<string, string | undefined>> = process.env,
): readonly string[] {
  if (path.length === 0 || path.includes("\0")) throw new Error("Editor path is invalid")
  const configured = environment.VISUAL?.trim() || environment.EDITOR?.trim() || "vi"
  const command = splitEditorCommand(configured)
  if (command.length === 0) throw new Error("Editor command is empty")
  return [...command, "--", path]
}

export async function openEditor(path: string): Promise<void> {
  const process = Bun.spawn({
    cmd: [...editorCommand(path)],
    stdin: "inherit",
    stdout: "inherit",
    stderr: "inherit",
  })
  const exitCode = await process.exited
  if (exitCode !== 0) throw new Error(`Editor exited with status ${exitCode}`)
}

export function openFile(path: string, dependencies?: OpenFileDependencies): Promise<void> {
  return launchOpenFile(path, dependencies)
}

export function openResolvedFile(argv: readonly string[]): Promise<void> {
  return launchResolvedOpenFile(argv)
}

export async function copyText(text: string): Promise<void> {
  await copyTextToClipboard(text)
}

export function toUiErrorMessage(error: unknown): string {
  if (error instanceof CoreClientError) {
    const details: string[] = [error.kind]
    if (error.coreCode !== undefined) {
      details.push(error.coreCode)
    }
    if (error.exitCode !== undefined) {
      details.push(`exit ${error.exitCode}`)
    }
    if (error.stderrTruncated) {
      details.push("diagnostics truncated")
    }
    return sanitizeDisplayText(`[${details.join(" · ")}] ${error.message}`)
  }
  return sanitizeDisplayText(error instanceof Error ? error.message : String(error))
}

export function exitCodeFor(error: unknown): number {
  if (error instanceof CoreClientError) {
    if (error.kind === "cancelled") {
      return 130
    }
    if (error.exitCode !== undefined && error.exitCode !== 0) {
      return error.exitCode
    }
  }
  return 1
}

export async function checkCore(
  executable: string,
  targetPath: string = process.cwd(),
  dependencies: CheckDependencies = { startCoreSession },
): Promise<CoreCheckResult> {
  const timeoutMs = dependencies.timeoutMs ?? 10_000
  if (!Number.isSafeInteger(timeoutMs) || timeoutMs <= 0 || timeoutMs > 120_000) {
    throw new RangeError("check timeout must be a positive safe integer no greater than 120000")
  }
  const controller = new AbortController()
  let state: ProbeState = initialProbeState()
  let resolveReady!: (result: CoreCheckResult) => void
  let rejectReady!: (error: unknown) => void
  let settled = false
  const ready = new Promise<CoreCheckResult>((resolve, reject) => {
    resolveReady = resolve
    rejectReady = reject
  })
  const finish = (callback: () => void) => {
    if (settled) return
    settled = true
    callback()
  }
  const timeout = setTimeout(() => {
    finish(() => rejectReady(new CoreClientError(
      `Core package check timed out after ${timeoutMs} ms`,
      { kind: "timeout" },
    )))
    controller.abort()
  }, timeoutMs)
  let session: ReturnType<typeof startCoreSession>
  try {
    session = dependencies.startCoreSession({
      executable,
      leftPath: targetPath,
      rightPath: targetPath,
      resume: false,
      persist: false,
      signal: controller.signal,
      onRecord: (record) => {
        try {
          const nextState = reduceProbeState(state, record)
          state = nextState
          if (nextState.phase === "failed") {
            finish(() => rejectReady(new CoreClientError(nextState.error.message, {
              kind: "core",
              coreCode: nextState.error.code,
            })))
          } else if (nextState.phase === "ready" && "session" in nextState) {
            if (nextState.version !== 3) {
              finish(() => rejectReady(new CoreClientError(
                `Portable preview requires protocol v3, received v${nextState.version}`,
                { kind: "protocol" },
              )))
              return
            }
            finish(() => resolveReady({
              protocolVersion: 3,
              implementation: nextState.hello.implementation,
              capabilities: nextState.hello.capabilities,
            }))
          }
        } catch (error) {
          finish(() => rejectReady(error))
        }
      },
      onError: (error) => finish(() => rejectReady(error)),
    })
  } catch (error) {
    clearTimeout(timeout)
    controller.abort()
    throw error
  }

  void session.completion.then(() => {
    finish(() => rejectReady(new CoreClientError(
      "Core session exited before publishing an initial v3 workspace",
      { kind: "exit" },
    )))
  }).catch((error) => finish(() => rejectReady(error)))

  try {
    return await ready
  } finally {
    clearTimeout(timeout)
    session.close()
    controller.abort()
    void session.completion.catch(() => undefined)
  }
}

export function appPropsFor(state: ProbeState, clientError?: string): AppProps {
	const session = state.phase === "ready" && "session" in state ? state : undefined
  return {
    loading: state.phase === "awaiting-hello" || state.phase === "awaiting-terminal",
    workspace: state.phase === "ready" && "workspace" in state ? state.workspace : undefined,
    error: clientError ?? (state.phase === "failed" ? state.error.message : undefined),
    preview: session?.preview,
    tasks: session?.tasks,
		actionTasks: session?.actionTasks,
		resourceTasks: session?.resourceTasks,
    open: session?.open,
    commandError: session?.commandError?.message,
    capabilities: state.phase === "ready" ? state.hello.capabilities : undefined,
  }
}

export async function main(
  args: readonly string[] = process.argv.slice(2),
  dependencies: MainDependencies = DEFAULT_MAIN_DEPENDENCIES,
): Promise<void> {
  const executable = process.env.NEOVIFM_CORE_SESSION?.trim()
    || process.env.NEOVIFM_CORE_PROBE?.trim()
    || dependencies.defaultCoreProbePath()
  const resume = args.length === 0
  const targetPath = args[0] ?? process.cwd()
  const rightPath = args[1] ?? targetPath
  const controller = new AbortController()
  const [probeState, setProbeState] = createSignal<ProbeState>(initialProbeState())
  const [clientError, setClientError] = createSignal<string | undefined>()

  const session = dependencies.startCoreSession({
    executable,
    leftPath: targetPath,
    rightPath,
    resume,
    persist: true,
    signal: controller.signal,
    onRecord: (record) => setProbeState((state) => reduceProbeState(state, record)),
    onError: (error) => {
      process.exitCode = exitCodeFor(error)
      setClientError(toUiErrorMessage(error))
    },
  })

  try {
    await dependencies.renderApp(() => {
      const state = probeState()
      return (
        {
          ...appPropsFor(state, clientError()),
          onCancel: () => { session.close() },
          onCommand: (command) => session.send(command),
          onEdit: dependencies.openEditor ?? openEditor,
          onOpen: dependencies.openFile ?? openFile,
          onOpenResolved: dependencies.openResolved ?? openResolvedFile,
          onCopyText: dependencies.copyText ?? copyText,
        }
      )
    })
  } catch (error) {
    controller.abort()
    session.close()
    await session.completion
    process.exitCode = exitCodeFor(error)
    throw error
  }
  session.close()
  await session.completion
}

const DEFAULT_CLI_RUNTIME: CliRuntime = {
  stdout: (line) => { console.log(line) },
  stderr: (line) => { console.error(line) },
  cwd: () => process.cwd(),
}

export async function runCli(
  args: readonly string[] = process.argv.slice(2),
  dependencies: MainDependencies = DEFAULT_MAIN_DEPENDENCIES,
  runtime: CliRuntime = DEFAULT_CLI_RUNTIME,
): Promise<number> {
  let parsed: CliArguments
  try {
    parsed = parseCliArgs(args)
  } catch (error) {
    runtime.stderr(toUiErrorMessage(error))
    runtime.stderr("Run 'neovifm --help' for usage.")
    return 2
  }

  if (parsed.action === "help") {
    runtime.stdout(cliHelp())
    return 0
  }
  if (parsed.action === "version") {
    runtime.stdout(cliVersion())
    return 0
  }
  if (parsed.action === "check") {
    try {
      const result = await checkCore(
        dependencies.defaultCoreProbePath(),
        runtime.cwd(),
        { startCoreSession: dependencies.startCoreSession },
      )
      runtime.stdout(`${cliVersion()} · core ${result.implementation} · protocol v${result.protocolVersion} · ok`)
      return 0
    } catch (error) {
      runtime.stderr(`NeoVifm package check failed: ${toUiErrorMessage(error)}`)
      return exitCodeFor(error)
    }
  }

  await main(parsed.paths, dependencies)
  const exitCode = process.exitCode
  return typeof exitCode === "number" ? exitCode : exitCode == null ? 0 : Number.parseInt(exitCode, 10)
}

if (import.meta.main) {
	process.exitCode = await runCli()
}
