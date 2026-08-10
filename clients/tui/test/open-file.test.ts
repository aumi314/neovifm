import { describe, expect, test } from "bun:test"

import {
  openCommand,
  openCommandForAssociation,
  openResolvedFile,
  validateOpenPath,
} from "../src/open-file.js"

describe("system open command", () => {
  test("uses the absolute macOS opener and keeps the target as one argv item", () => {
    expect(openCommand("/tmp/file name.pdf", "darwin")).toEqual([
      "/usr/bin/open",
      "/tmp/file name.pdf",
    ])
  })

  test("uses platform fallbacks without a shell", () => {
    expect(openCommand("/tmp/file name.pdf", "linux")).toEqual(["xdg-open", "/tmp/file name.pdf"])
    expect(() => openCommand("C:\\Temp\\file name.pdf", "win32")).toThrow("Windows opener requires open-v1")
  })

  test("accepts a core-provided Vifm association as argv", () => {
    expect(openCommandForAssociation("/tmp/file name.pdf", ["zathura", "--fork"])).toEqual([
      "zathura",
      "--fork",
      "/tmp/file name.pdf",
    ])
  })

  test("rejects empty, NUL-containing, and empty association arguments", () => {
    expect(() => validateOpenPath("")).toThrow("Open path is invalid")
    expect(() => validateOpenPath("/tmp/a\0b")).toThrow("Open path is invalid")
    expect(() => openCommandForAssociation("/tmp/file", [])).toThrow("Open association is empty")
    expect(() => openCommandForAssociation("/tmp/file", ["helper", "bad\0arg"])).toThrow("Open association is invalid")
  })

  test("executes a core-resolved argv without rebuilding or shelling it", async () => {
    let command: readonly string[] | undefined
    await openResolvedFile(["/usr/bin/open", "--background", "/tmp/file name.pdf"], {
      spawn: (options) => {
        command = options.cmd
        return { exited: Promise.resolve(0) }
      },
    })
    expect(command).toEqual(["/usr/bin/open", "--background", "/tmp/file name.pdf"])
    await expect(openResolvedFile(["viewer", "bad\0arg"], {
      spawn: () => ({ exited: Promise.resolve(0) }),
    })).rejects.toThrow("Open command argument is invalid")
  })

  test("includes bounded sanitized stderr when a resolved opener fails", async () => {
    const diagnostics = new ReadableStream<Uint8Array>({
      start(controller) {
        controller.enqueue(new TextEncoder().encode(`No association\u001b[31m${"x".repeat(5000)}`))
        controller.close()
      },
    })
    let message = ""
    try {
      await openResolvedFile(["neovifm-win-open.exe", "C:\\Temp\\note.txt"], {
        spawn: () => ({ exited: Promise.resolve(7), stderr: diagnostics }),
      })
    } catch (error) {
      message = error instanceof Error ? error.message : String(error)
    }
    expect(message).toContain("Open exited with status 7: No association�[31m")
    expect(message.length).toBeLessThan(4200)
  })

  test("reports a missing resolved opener without trying another command", async () => {
    await expect(openResolvedFile(["C:\\missing\\neovifm-win-open.exe", "C:\\Temp\\note.txt"], {
      spawn: () => { throw new Error("ENOENT") },
    })).rejects.toThrow("Open failed: ENOENT")
  })
})
