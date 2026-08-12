import { createHash } from "node:crypto"
import { createReadStream } from "node:fs"
import { chmod, copyFile, mkdir, readdir, readFile, rm, stat, writeFile } from "node:fs/promises"
import { basename, join, relative, resolve } from "node:path"

interface StageOptions {
  readonly output: string
  readonly platform: "linux" | "macos" | "windows"
  readonly arch: "x64" | "arm64"
  readonly tui: string
  readonly core: string
  readonly helper?: string
  readonly dependencies: string
  readonly runtimeDlls: readonly string[]
}

interface PackageMetadata {
  readonly name: string
  readonly version: string
  readonly license: string
  readonly repository?: unknown
}

const projectRoot = resolve(import.meta.dir, "../..")
const tuiRoot = join(projectRoot, "clients", "tui")

function parseOptions(args: readonly string[]): StageOptions {
  const values = new Map<string, string[]>()
  for (let index = 0; index < args.length; index += 2) {
    const name = args[index]
    const value = args[index + 1]
    if (name === undefined || !name.startsWith("--") || value === undefined) {
      throw new Error("Preview stage arguments must be --name value pairs")
    }
    values.set(name, [...(values.get(name) ?? []), value])
  }
  const required = (name: string): string => {
    const value = values.get(name)?.at(-1)?.trim()
    if (!value) throw new Error(`Missing ${name}`)
    return value
  }
  const platform = required("--platform")
  const arch = required("--arch")
  if (platform !== "linux" && platform !== "macos" && platform !== "windows") {
    throw new Error(`Unsupported preview platform: ${platform}`)
  }
  if (arch !== "x64" && arch !== "arm64") throw new Error(`Unsupported preview architecture: ${arch}`)
  return {
    output: resolve(required("--output")),
    platform,
    arch,
    tui: resolve(required("--tui")),
    core: resolve(required("--core")),
    dependencies: resolve(required("--dependencies")),
    ...(values.has("--helper") ? { helper: resolve(required("--helper")) } : {}),
    runtimeDlls: (values.get("--runtime-dll") ?? []).map((path) => resolve(path)),
  }
}

async function git(...args: readonly string[]): Promise<string> {
  const process = Bun.spawn({ cmd: ["git", "-C", projectRoot, ...args], stdout: "pipe", stderr: "pipe" })
  const [code, stdout, stderr] = await Promise.all([
    process.exited,
    new Response(process.stdout).text(),
    new Response(process.stderr).text(),
  ])
  if (code !== 0) throw new Error(`git ${args.join(" ")} failed: ${stderr.trim()}`)
  return stdout.trim()
}

async function assertFile(path: string, label: string): Promise<void> {
  const details = await stat(path).catch(() => undefined)
  if (details === undefined || !details.isFile()) throw new Error(`${label} is not a file: ${path}`)
}

async function copyLicenses(destination: string): Promise<readonly PackageMetadata[]> {
  await mkdir(destination, { recursive: true })
  await Promise.all([
    copyFile(join(projectRoot, "COPYING"), join(destination, "NEOVIFM-GPL-2.0-or-later.txt")),
    copyFile(join(projectRoot, "COPYING.3party"), join(destination, "VIFM-THIRD-PARTY.txt")),
    copyFile(join(projectRoot, "licenses", "preview", "BUN-1.3.10.md"), join(destination, "BUN-1.3.10.md")),
    copyFile(join(projectRoot, "licenses", "preview", "OPENTUI-0.4.3.txt"), join(destination, "OPENTUI-0.4.3.txt")),
  ])

  const packages = new Map<string, PackageMetadata>()
  const scanNodeModules = async (root: string): Promise<void> => {
    for (const entry of await readdir(root, { withFileTypes: true }).catch(() => [])) {
      if (!entry.isDirectory() || entry.name === ".bin") continue
      const first = join(root, entry.name)
      const directories = entry.name.startsWith("@")
        ? (await readdir(first, { withFileTypes: true })).filter((child) => child.isDirectory()).map((child) => join(first, child.name))
        : [first]
      for (const directory of directories) {
        const raw = await readFile(join(directory, "package.json"), "utf8").catch(() => undefined)
        if (raw === undefined) continue
        const value = JSON.parse(raw) as Partial<PackageMetadata>
        if (!value.name || !value.version || !value.license) {
          throw new Error(`npm package metadata lacks name, version, or license: ${directory}`)
        }
        const metadata: PackageMetadata = {
          name: value.name,
          version: value.version,
          license: value.license,
          ...(value.repository === undefined ? {} : { repository: value.repository }),
        }
        const key = `${metadata.name}@${metadata.version}`
        if (!packages.has(key)) {
          packages.set(key, metadata)
          const packageDestination = join(destination, "npm", key.replaceAll("/", "__"))
          await mkdir(packageDestination, { recursive: true })
          let copied = 0
          for (const file of await readdir(directory, { withFileTypes: true })) {
            if (file.isFile() && /^(?:licen[cs]e|copying|notice)(?:\.|$)/i.test(file.name)) {
              await copyFile(join(directory, file.name), join(packageDestination, file.name))
              copied += 1
            }
          }
          await writeFile(join(packageDestination, "PACKAGE.json"), `${JSON.stringify(metadata, null, 2)}\n`)
          if (copied === 0) await writeFile(join(packageDestination, "LICENSE-SPDX.txt"), `${metadata.license}\n`)
        }
        await scanNodeModules(join(directory, "node_modules"))
      }
    }
  }
  await scanNodeModules(join(tuiRoot, "node_modules"))
  const result = [...packages.values()].sort((left, right) => `${left.name}@${left.version}`.localeCompare(`${right.name}@${right.version}`))
  await writeFile(join(destination, "NPM-PACKAGES.json"), `${JSON.stringify(result, null, 2)}\n`)
  return result
}

async function hashFile(path: string): Promise<string> {
  const hash = createHash("sha256")
  for await (const chunk of createReadStream(path)) hash.update(chunk)
  return hash.digest("hex")
}

async function filesBelow(root: string, current = root): Promise<string[]> {
  const result: string[] = []
  for (const entry of await readdir(current, { withFileTypes: true })) {
    const path = join(current, entry.name)
    if (entry.isDirectory()) result.push(...await filesBelow(root, path))
    else if (entry.isFile()) result.push(relative(root, path).replaceAll("\\", "/"))
  }
  return result
}

async function main(): Promise<void> {
  const options = parseOptions(process.argv.slice(2))
  await assertFile(options.tui, "TUI executable")
  await assertFile(options.core, "Core session")
  await assertFile(options.dependencies, "Dependency report")
  if (options.platform === "windows" && options.helper === undefined) throw new Error("Windows preview requires --helper")
  if (options.helper !== undefined) await assertFile(options.helper, "Windows open helper")
  for (const dll of options.runtimeDlls) await assertFile(dll, "Runtime DLL")

  const commit = await git("rev-parse", "HEAD")
  if (!/^[0-9a-f]{40}$/.test(commit)) throw new Error(`Invalid source commit: ${commit}`)
  const shortCommit = commit.slice(0, 12)
  const packageName = `neovifm-workbench-alpha0-${options.platform}-${options.arch}-${shortCommit}`
  const packageRoot = join(options.output, packageName)
  await rm(packageRoot, { recursive: true, force: true })
  await mkdir(packageRoot, { recursive: true })

  const suffix = options.platform === "windows" ? ".exe" : ""
  const tuiExecutable = join(packageRoot, `neovifm${suffix}`)
  const coreExecutable = join(packageRoot, `neovifm-core-session${suffix}`)
  await copyFile(options.tui, tuiExecutable)
  await copyFile(options.core, coreExecutable)
  if (options.platform !== "windows") await Promise.all([chmod(tuiExecutable, 0o755), chmod(coreExecutable, 0o755)])
  if (options.helper !== undefined) await copyFile(options.helper, join(packageRoot, "neovifm-win-open.exe"))
  if (options.runtimeDlls.length > 0) {
    const runtime = join(packageRoot, "runtime")
    await mkdir(runtime)
    for (const dll of options.runtimeDlls) await copyFile(dll, join(runtime, basename(dll)))
  }

  const sourceDirectory = join(packageRoot, "source")
  await mkdir(sourceDirectory)
  const sourceArchive = join(sourceDirectory, `neovifm-${commit}.tar.gz`)
  await git("archive", "--format=tar.gz", `--prefix=neovifm-${commit}/`, `--output=${sourceArchive}`, "HEAD")

  const packages = await copyLicenses(join(packageRoot, "LICENSES"))
  const dependencyLines = (await readFile(options.dependencies, "utf8"))
    .split(/\r?\n/u).map((line) => line.trim()).filter(Boolean)
  const packageJson = JSON.parse(await readFile(join(tuiRoot, "package.json"), "utf8")) as { dependencies: Record<string, string> }
  const buildInfo = {
    stage: "Workbench Alpha 0 (unreleased)",
    source_commit: commit,
    platform: options.platform,
    architecture: options.arch,
    bun: Bun.version,
    opentui: packageJson.dependencies["@opentui/core"],
    solid: packageJson.dependencies["solid-js"],
    runtime_dependencies: dependencyLines,
    bundled_runtime_dlls: options.runtimeDlls.map((path) => basename(path)).sort(),
    npm_package_count: packages.length,
  }
  await writeFile(join(packageRoot, "BUILD-INFO.json"), `${JSON.stringify(buildInfo, null, 2)}\n`)
  const warning = options.platform === "windows"
    ? "This archive is unsigned; Windows SmartScreen can warn before first use."
    : options.platform === "macos"
      ? "This archive is unsigned and not notarized; macOS Gatekeeper can block first use."
      : "This build targets glibc-compatible x64 Linux systems."
  await writeFile(join(packageRoot, "README.txt"), [
    `NeoVifm Workbench Alpha 0 (unreleased) ${shortCommit}`,
    "",
    `Run: ./neovifm${suffix} [LEFT [RIGHT]]`,
    `Check: ./neovifm${suffix} --check`,
    "",
    warning,
    "This is a temporary portable preview, not a stable release or installer.",
    "Optional preview and resource helpers such as chafa, ffmpeg, sshfs and archivemount are not bundled.",
    "License texts and the exact corresponding NeoVifm source are included in this directory.",
    "",
  ].join("\n"))

  const checksummed = (await filesBelow(packageRoot)).filter((path) => path !== "SHA256SUMS").sort()
  const checksumLines: string[] = []
  for (const path of checksummed) checksumLines.push(`${await hashFile(join(packageRoot, path))}  ${path}`)
  await writeFile(join(packageRoot, "SHA256SUMS"), `${checksumLines.join("\n")}\n`)
  console.log(packageRoot)
}

await main()
