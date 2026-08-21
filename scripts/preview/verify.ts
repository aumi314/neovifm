import { createHash } from "node:crypto"
import { createReadStream } from "node:fs"
import { readdir, readFile, rename, stat } from "node:fs/promises"
import { basename, dirname, join, relative, resolve, sep } from "node:path"

interface VerifyOptions {
  readonly packageRoot: string
  readonly outsideCwd: string
  readonly forbiddenPaths: readonly string[]
}

interface BuildInfo {
  readonly stage: string
  readonly source_commit: string
  readonly platform: "linux" | "macos" | "windows"
  readonly architecture: "x64" | "arm64"
  readonly bun: string
  readonly opentui: string
  readonly solid: string
  readonly runtime_dependencies: readonly string[]
  readonly bundled_runtime_dlls: readonly string[]
}

function parseOptions(args: readonly string[]): VerifyOptions {
  const values = new Map<string, string[]>()
  for (let index = 0; index < args.length; index += 2) {
    const name = args[index]
    const value = args[index + 1]
    if (name === undefined || !name.startsWith("--") || value === undefined) {
      throw new Error("Preview verification arguments must be --name value pairs")
    }
    values.set(name, [...(values.get(name) ?? []), value])
  }
  const required = (name: string): string => {
    const value = values.get(name)?.at(-1)?.trim()
    if (!value) throw new Error(`Missing ${name}`)
    return value
  }
  return {
    packageRoot: resolve(required("--package")),
    outsideCwd: resolve(required("--cwd")),
    forbiddenPaths: (values.get("--forbidden-path") ?? []).map((path) => resolve(path)),
  }
}

async function filesBelow(root: string, current = root): Promise<string[]> {
  const result: string[] = []
  for (const entry of await readdir(current, { withFileTypes: true })) {
    const path = join(current, entry.name)
    if (entry.isDirectory()) result.push(...await filesBelow(root, path))
    else if (entry.isFile()) result.push(relative(root, path).replaceAll("\\", "/"))
    else throw new Error(`Package contains a non-file entry: ${path}`)
  }
  return result
}

async function hashFile(path: string): Promise<string> {
  const hash = createHash("sha256")
  for await (const chunk of createReadStream(path)) hash.update(chunk)
  return hash.digest("hex")
}

async function run(command: readonly string[], cwd: string, timeoutMs = 20_000): Promise<{ code: number; stdout: string; stderr: string }> {
  const controller = new AbortController()
  const timeout = setTimeout(() => controller.abort(), timeoutMs)
  const env = { ...process.env }
  delete env.NEOVIFM_CORE_SESSION
  delete env.NEOVIFM_CORE_PROBE
  try {
    const child = Bun.spawn({ cmd: [...command], cwd, env, signal: controller.signal, stdout: "pipe", stderr: "pipe" })
    const [code, stdout, stderr] = await Promise.all([
      child.exited,
      new Response(child.stdout).text(),
      new Response(child.stderr).text(),
    ])
    return { code, stdout, stderr }
  } catch (error) {
    if (controller.signal.aborted) throw new Error(`Command timed out: ${command.join(" ")}`)
    throw error
  } finally {
    clearTimeout(timeout)
  }
}

function assert(condition: unknown, message: string): asserts condition {
  if (!condition) throw new Error(message)
}

async function main(): Promise<void> {
  const options = parseOptions(process.argv.slice(2))
  const rootDetails = await stat(options.packageRoot).catch(() => undefined)
  assert(rootDetails?.isDirectory(), `Package root is not a directory: ${options.packageRoot}`)
  const cwdDetails = await stat(options.outsideCwd).catch(() => undefined)
  assert(cwdDetails?.isDirectory(), `Verification cwd is not a directory: ${options.outsideCwd}`)
  assert(!options.outsideCwd.startsWith(`${options.packageRoot}${sep}`), "Verification cwd must be outside the package")

  const buildInfo = JSON.parse(await readFile(join(options.packageRoot, "BUILD-INFO.json"), "utf8")) as BuildInfo
  assert(buildInfo.stage === "Workbench Alpha 0 (unreleased)", "Unexpected preview stage")
  assert(/^[0-9a-f]{40}$/.test(buildInfo.source_commit), "Invalid source commit in BUILD-INFO.json")
  assert(buildInfo.bun === "1.3.10", `Unexpected Bun version: ${buildInfo.bun}`)
  assert(buildInfo.opentui === "0.4.3", `Unexpected OpenTUI version: ${buildInfo.opentui}`)
  assert(buildInfo.solid === "1.9.12", `Unexpected SolidJS version: ${buildInfo.solid}`)
  assert(buildInfo.runtime_dependencies.length > 0, "Dynamic dependency audit is empty")
  const expectedName = `neovifm-workbench-alpha0-${buildInfo.platform}-${buildInfo.architecture}-${buildInfo.source_commit.slice(0, 12)}`
  assert(basename(options.packageRoot) === expectedName, `Package directory does not match BUILD-INFO.json: ${expectedName}`)

  const suffix = buildInfo.platform === "windows" ? ".exe" : ""
  const expectedTopLevel = new Set([
    "BUILD-INFO.json", "LICENSES", "README.txt", "SHA256SUMS", `neovifm${suffix}`,
    `neovifm-core-session${suffix}`, "source",
    ...(buildInfo.platform === "windows" ? ["neovifm-win-open.exe"] : []),
    ...(buildInfo.bundled_runtime_dlls.length > 0 ? ["runtime"] : []),
  ])
  const topLevel = await readdir(options.packageRoot)
  assert(topLevel.every((entry) => expectedTopLevel.has(entry)), `Unexpected top-level package entry: ${topLevel.filter((entry) => !expectedTopLevel.has(entry)).join(", ")}`)
  assert([...expectedTopLevel].every((entry) => topLevel.includes(entry)), `Package is missing required entries: ${[...expectedTopLevel].filter((entry) => !topLevel.includes(entry)).join(", ")}`)

  const files = (await filesBelow(options.packageRoot)).sort()
  assert(!files.some((path) => path.includes("node_modules/") || /(^|\/)neovifm-core-probe(?:\.exe)?$/u.test(path) || /(^|\/)vifm(?:\.exe)?$/u.test(path)), "Package contains a forbidden development or classic binary")
  for (const required of [
    "LICENSES/BUN-1.3.10.md", "LICENSES/NEOVIFM-GPL-2.0-or-later.txt", "LICENSES/NPM-PACKAGES.json",
    "LICENSES/OPENTUI-0.4.3.txt", "LICENSES/VIFM-THIRD-PARTY.txt",
    `source/neovifm-${buildInfo.source_commit}.tar.gz`,
  ]) assert(files.includes(required), `Package is missing ${required}`)

  const expectedDlls = buildInfo.bundled_runtime_dlls.map((name) => `runtime/${name}`).sort()
  const packagedDlls = files.filter((path) => path.startsWith("runtime/")).sort()
  assert(JSON.stringify(packagedDlls) === JSON.stringify(expectedDlls), "Bundled runtime DLLs do not match BUILD-INFO.json")

  const checksumText = await readFile(join(options.packageRoot, "SHA256SUMS"), "utf8")
  const checksums = new Map<string, string>()
  for (const line of checksumText.trim().split(/\r?\n/u)) {
    const match = /^([0-9a-f]{64})  ([^\0]+)$/u.exec(line)
    assert(match !== null, `Invalid SHA256SUMS line: ${line}`)
    const path = match[2]!
    assert(!path.startsWith("/") && !path.split("/").includes(".."), `Unsafe checksum path: ${path}`)
    assert(!checksums.has(path), `Duplicate checksum path: ${path}`)
    checksums.set(path, match[1]!)
  }
  const checksummedFiles = files.filter((path) => path !== "SHA256SUMS").sort()
  assert(JSON.stringify([...checksums.keys()].sort()) === JSON.stringify(checksummedFiles), "SHA256SUMS file list does not match package contents")
  for (const [path, expected] of checksums) {
    assert(await hashFile(join(options.packageRoot, path)) === expected, `Checksum mismatch: ${path}`)
  }

  const dependencyText = buildInfo.runtime_dependencies.join("\n").toLowerCase()
  for (const forbidden of options.forbiddenPaths) {
    const normalized = forbidden.replaceAll("\\", "/").toLowerCase()
    assert(!dependencyText.replaceAll("\\", "/").includes(normalized), `Dynamic dependency audit contains workspace path: ${forbidden}`)
  }

  const sourceArchive = join(options.packageRoot, "source", `neovifm-${buildInfo.source_commit}.tar.gz`)
  const archive = await run(["tar", "-tzf", basename(sourceArchive)], dirname(sourceArchive))
  assert(archive.code === 0, `Source archive is invalid: ${archive.stderr.trim()}`)
  const sourcePrefix = `neovifm-${buildInfo.source_commit}/`
  assert(archive.stdout.split(/\r?\n/u).filter(Boolean).every((path) => path.startsWith(sourcePrefix)), "Source archive contains an unexpected root")

  const executable = join(options.packageRoot, `neovifm${suffix}`)
  const core = join(options.packageRoot, `neovifm-core-session${suffix}`)
  const version = await run([executable, "--version"], options.outsideCwd)
  assert(version.code === 0 && version.stdout.includes(buildInfo.source_commit.slice(0, 12)), `--version failed: ${version.stderr.trim()}`)
  const help = await run([executable, "--help"], options.outsideCwd)
  assert(help.code === 0 && help.stdout.includes("neovifm [LEFT [RIGHT]]"), `--help failed: ${help.stderr.trim()}`)
  const initialCheck = await run([executable, "--check"], options.outsideCwd)
  assert(initialCheck.code === 0 && initialCheck.stdout.includes("protocol v3") && initialCheck.stdout.includes(" ok"), `Initial --check failed: ${initialCheck.stderr.trim()}`)

  const missingCore = `${core}.missing`
  await rename(core, missingCore)
  try {
    const failedCheck = await run([executable, "--check"], options.outsideCwd)
    assert(failedCheck.code !== 0 && failedCheck.stderr.includes("package check failed"), "--check did not fail clearly when the sibling core was missing")
  } finally {
    await rename(missingCore, core)
  }
  const restoredCheck = await run([executable, "--check"], options.outsideCwd)
  assert(restoredCheck.code === 0, `Restored --check failed: ${restoredCheck.stderr.trim()}`)
  console.log(`Verified ${expectedName}`)
}

await main()
