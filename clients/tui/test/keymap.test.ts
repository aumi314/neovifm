import { expect, test } from "bun:test"

import { VifmKeymap, type KeyLike } from "../src/keymap.js"

const key = (name: string, options: Partial<KeyLike> = {}): KeyLike => ({
  name,
  sequence: options.sequence ?? name,
  ctrl: options.ctrl ?? false,
  shift: options.shift ?? false,
  meta: options.meta ?? false,
})

test("maps all arrow keys to the matching Vifm h/j/k/l navigation", () => {
  const map = new VifmKeymap()
  expect(map.handle(key("h"))).toEqual({ kind: "command", command: { action: "parent" } })
  expect(map.handle(key("j"))).toEqual({ kind: "command", command: { action: "move", delta: 1 } })
  expect(map.handle(key("k"))).toEqual({ kind: "command", command: { action: "move", delta: -1 } })
  expect(map.handle(key("l"))).toEqual({ kind: "command", command: { action: "enter" } })
  expect(map.handle(key("down", { sequence: "\u001b[B" }))).toEqual({ kind: "command", command: { action: "move", delta: 1 } })
  expect(map.handle(key("up", { sequence: "\u001b[A" }))).toEqual({ kind: "command", command: { action: "move", delta: -1 } })
  expect(map.handle(key("left", { sequence: "\u001b[D" }))).toEqual({ kind: "command", command: { action: "parent" } })
  expect(map.handle(key("right", { sequence: "\u001b[C" }))).toEqual({ kind: "command", command: { action: "enter" } })
})

test("keeps Tab for pane switching and reserves Space for opposite-pane preview", () => {
  const map = new VifmKeymap()
  expect(map.handle(key("space", { sequence: " " }))).toEqual({ kind: "function", action: "quick-view" })
  expect(map.handle(key("tab", { sequence: "\t" }))).toEqual({ kind: "command", command: { action: "focus-next" } })
  expect(map.handle(key("n", { ctrl: true }))).toEqual({ kind: "command", command: { action: "move", delta: 1 } })
  expect(map.handle(key("p", { ctrl: true }))).toEqual({ kind: "command", command: { action: "move", delta: -1 } })
})

test("maps Vifm gg/G and Ctrl-W pane sequences", () => {
  const map = new VifmKeymap()
  expect(map.handle(key("g"))).toEqual({ kind: "pending" })
  expect(map.handle(key("g"))).toEqual({ kind: "command", command: { action: "move-to", target: "first" } })
  expect(map.handle(key("g", { shift: true, sequence: "G" }))).toEqual({ kind: "command", command: { action: "move-to", target: "last" } })
  expect(map.handle(key("w", { ctrl: true }))).toEqual({ kind: "pending" })
  expect(map.handle(key("w"))).toEqual({ kind: "command", command: { action: "focus-next" } })
})

test("maps Vifm gt/gT tab navigation including counts", () => {
  const map = new VifmKeymap()
  expect(map.handle(key("g"))).toEqual({ kind: "pending" })
  expect(map.handle(key("t"))).toEqual({ kind: "command", command: { action: "tab-cycle", delta: 1 } })
  expect(map.handle(key("g"))).toEqual({ kind: "pending" })
  expect(map.handle(key("t", { shift: true, sequence: "T" }))).toEqual({
    kind: "command",
    command: { action: "tab-cycle", delta: -1 },
  })

  expect(map.handle(key("2"))).toEqual({ kind: "pending" })
  expect(map.handle(key("g"))).toEqual({ kind: "pending" })
  expect(map.handle(key("t"))).toEqual({ kind: "tab-index", index: 1 })
  expect(map.handle(key("3"))).toEqual({ kind: "pending" })
  expect(map.handle(key("g"))).toEqual({ kind: "pending" })
  expect(map.handle(key("t", { shift: true, sequence: "T" }))).toEqual({
    kind: "command",
    command: { action: "tab-cycle", delta: -3 },
  })

  expect(map.handle(key("9"))).toEqual({ kind: "pending" })
  expect(map.handle(key("9"))).toEqual({ kind: "pending" })
  expect(map.handle(key("9"))).toEqual({ kind: "pending" })
  expect(map.handle(key("g"))).toEqual({ kind: "pending" })
  expect(map.handle(key("t", { shift: true, sequence: "T" }))).toEqual({
    kind: "command",
    command: { action: "tab-cycle", delta: -999 },
  })
})

test("does not swallow a normal command after an unsupported numeric count", () => {
  const map = new VifmKeymap()
  expect(map.handle(key("2"))).toEqual({ kind: "pending" })
  expect(map.handle(key("j"))).toEqual({
    kind: "command",
    command: { action: "move", delta: 1 },
  })
  expect(map.handle(key("9"))).toEqual({ kind: "pending" })
  expect(map.handle(key("f3"))).toEqual({ kind: "function", action: "view" })
})

test("keeps shifted navigation and quit/refresh keys aligned with Vifm", () => {
  const map = new VifmKeymap()
  expect(map.handle(key("home"))).toEqual({ kind: "command", command: { action: "move-to", target: "first" } })
  expect(map.handle(key("end"))).toEqual({ kind: "command", command: { action: "move-to", target: "last" } })
  expect(map.handle(key("l", { ctrl: true }))).toEqual({ kind: "command", command: { action: "refresh" } })
  expect(map.handle(key("h", { shift: true, sequence: "H" }))).toEqual({ kind: "unhandled" })
  expect(map.handle(key("l", { shift: true, sequence: "L" }))).toEqual({ kind: "unhandled" })
  expect(map.handle(key("q"))).toEqual({ kind: "pending" })
  expect(map.handle(key("j"))).toEqual({ kind: "unhandled" })
  expect(map.handle(key("r"))).toEqual({ kind: "unhandled" })
  expect(map.handle(key("c", { ctrl: true }))).toEqual({ kind: "unhandled" })
  expect(map.handle(key("z", { shift: true, sequence: "Z" }))).toEqual({ kind: "pending" })
  expect(map.handle(key("q", { shift: true, sequence: "Q" }))).toEqual({ kind: "cancel" })
  expect(map.handle(key("z", { shift: true, sequence: "Z" }))).toEqual({ kind: "pending" })
  expect(map.handle(key("z", { shift: true, sequence: "Z" }))).toEqual({ kind: "cancel" })
})

test("falls back to sequence when a terminal leaves key name empty", () => {
  const map = new VifmKeymap()
  expect(map.handle(key("", { sequence: "j" }))).toEqual({ kind: "command", command: { action: "move", delta: 1 } })
  expect(map.handle(key("", { sequence: "\t" }))).toEqual({ kind: "command", command: { action: "focus-next" } })
  expect(map.handle(key("", { sequence: " " }))).toEqual({ kind: "function", action: "quick-view" })
  expect(map.handle(key("", { sequence: "\u001b[A" }))).toEqual({ kind: "unhandled" })
})

test("exposes every Total Commander function key through the shared action dispatcher", () => {
  const map = new VifmKeymap()
  expect(map.handle(key("f3"))).toEqual({ kind: "function", action: "view" })
  expect(map.handle(key("f4"))).toEqual({ kind: "function", action: "edit" })
  expect(map.handle(key("f5"))).toEqual({ kind: "function", action: "copy" })
  expect(map.handle(key("f6"))).toEqual({ kind: "function", action: "move" })
  expect(map.handle(key("f7"))).toEqual({ kind: "function", action: "mkdir" })
  expect(map.handle(key("f8"))).toEqual({ kind: "function", action: "delete" })
  expect(map.handle(key("f9"))).toEqual({ kind: "function", action: "mount-ssh" })
  expect(map.handle(key("f10"))).toEqual({ kind: "function", action: "quit" })
})

test("maps Vifm register verbs yy/Y and put p/P", () => {
  const map = new VifmKeymap()
  expect(map.handle(key("y"))).toEqual({ kind: "pending" })
  expect(map.handle(key("y"))).toEqual({ kind: "function", action: "yank" })
  expect(map.handle(key("y", { shift: true, sequence: "Y" }))).toEqual({ kind: "function", action: "yank" })
  expect(map.handle(key("p"))).toEqual({ kind: "function", action: "put-copy" })
  expect(map.handle(key("p", { shift: true, sequence: "P" }))).toEqual({ kind: "function", action: "put-move" })
})

test("maps Vifm dd to delete and keeps d/D as prefixes", () => {
  const map = new VifmKeymap()
  expect(map.handle(key("d"))).toEqual({ kind: "pending" })
  expect(map.handle(key("d"))).toEqual({ kind: "function", action: "delete" })

  expect(map.handle(key("d", { shift: true, sequence: "D" }))).toEqual({ kind: "pending" })
  expect(map.handle(key("d", { shift: true, sequence: "D" }))).toEqual({ kind: "unhandled" })

  expect(map.handle(key("d"))).toEqual({ kind: "pending" })
  expect(map.handle(key("j"))).toEqual({ kind: "unhandled" })
})

test("rejects stray y prefix continuations without disturbing later keys", () => {
  const map = new VifmKeymap()
  expect(map.handle(key("y"))).toEqual({ kind: "pending" })
  expect(map.handle(key("x"))).toEqual({ kind: "unhandled" })
  expect(map.handle(key("j"))).toEqual({ kind: "command", command: { action: "move", delta: 1 } })
})

test("clears a pending count for register verbs just like other plain keys", () => {
  const map = new VifmKeymap()
  expect(map.handle(key("2"))).toEqual({ kind: "pending" })
  expect(map.handle(key("p"))).toEqual({ kind: "function", action: "put-copy" })
  expect(map.handle(key("3"))).toEqual({ kind: "pending" })
  expect(map.handle(key("y"))).toEqual({ kind: "pending" })
  expect(map.handle(key("y"))).toEqual({ kind: "function", action: "yank" })
})

test("routes Vifm u through the core-owned undo command", () => {
  const map = new VifmKeymap()
  expect(map.handle(key("u"))).toEqual({ kind: "command", command: { action: "undo" } })
})

test("opens directional name search and repeats matches with n/N", () => {
  const map = new VifmKeymap()
  expect(map.handle(key("/"))).toEqual({ kind: "search", direction: 1 })
  expect(map.handle(key("?"))).toEqual({ kind: "search", direction: -1 })
  expect(map.handle(key("n"))).toEqual({ kind: "command", command: { action: "search-next", direction: 1 } })
  expect(map.handle(key("n", { shift: true, sequence: "N" }))).toEqual({
    kind: "command",
    command: { action: "search-next", direction: -1 },
  })
})
