import type { CoreSessionCommand } from "./core-client.js"

export interface KeyLike {
  readonly name: string
  readonly sequence: string
  readonly ctrl: boolean
  readonly shift: boolean
  readonly meta: boolean
}

export type FunctionAction = "view" | "quick-view" | "edit" | "copy" | "move" | "mkdir" | "delete" | "mount-ssh" | "quit" | "yank" | "put-copy" | "put-move"

export type KeymapResult =
  | Readonly<{ kind: "command"; command: CoreSessionCommand }>
  | Readonly<{ kind: "tab-index"; index: number }>
  | Readonly<{ kind: "search"; direction: -1 | 1 }>
  | Readonly<{ kind: "cancel" }>
  | Readonly<{ kind: "function"; action: FunctionAction }>
  | Readonly<{ kind: "pending" }>
  | Readonly<{ kind: "unhandled" }>

type Prefix = "g" | "q" | "ctrl-w" | "shift-z" | "d" | "y"

const command = (value: CoreSessionCommand): KeymapResult => ({ kind: "command", command: value })

function normalizedName(key: KeyLike): string {
  if (key.name.length !== 0) return key.name.toLowerCase()
  if (key.sequence === "\t") return "tab"
  if (key.sequence === " ") return "space"
  return key.sequence.length === 1 ? key.sequence.toLowerCase() : ""
}

export class VifmKeymap {
  #prefix: Prefix | undefined
  #count: number | undefined

  // Bun 1.3.10 does not count an implicit constructor as covered.
  constructor() {}

  handle(key: KeyLike): KeymapResult {
    const name = normalizedName(key)
    const prefix = this.#prefix
    this.#prefix = undefined

    if (prefix === "g") {
      const count = this.#count
      this.#count = undefined
      if (name === "t" && key.shift) {
        return command({ action: "tab-cycle", delta: -(count ?? 1) })
      }
      if (key.shift) return { kind: "unhandled" }
      if (name === "t") {
        return count === undefined
          ? command({ action: "tab-cycle", delta: 1 })
          : { kind: "tab-index", index: count - 1 }
      }
      if (name === "g") return command({ action: "move-to", target: "first" })
      if (name === "h") return command({ action: "parent" })
      if (name === "j") return command({ action: "move", delta: 1 })
      if (name === "k") return command({ action: "move", delta: -1 })
      if (name === "l") return command({ action: "enter" })
      return { kind: "unhandled" }
    }

    if (prefix === "q") {
      this.#count = undefined
      return { kind: "unhandled" }
    }

    if (prefix === "d") {
      this.#count = undefined
      // Vifm maps dd to remove; DD stays reserved for a future permanent delete.
      if (name === "d" && !key.shift) return { kind: "function", action: "delete" }
      return { kind: "unhandled" }
    }

    if (prefix === "y") {
      this.#count = undefined
      if (name === "y" && !key.shift) return { kind: "function", action: "yank" }
      return { kind: "unhandled" }
    }

    if (prefix === "shift-z") {
      this.#count = undefined
      if (key.shift && (name === "q" || name === "z")) return { kind: "cancel" }
      return { kind: "unhandled" }
    }

    if (prefix === "ctrl-w") {
      this.#count = undefined
      if (key.shift) return { kind: "unhandled" }
      if (name === "w" || name === "p") return command({ action: "focus-next" })
      if (name === "h") return command({ action: "focus", pane: "left" })
      if (name === "l") return command({ action: "focus", pane: "right" })
      return { kind: "unhandled" }
    }

    if (key.ctrl && name === "w") {
      this.#count = undefined
      this.#prefix = "ctrl-w"
      return { kind: "pending" }
    }
    if (key.ctrl && name === "l") return command({ action: "refresh" })
    if (key.ctrl && name === "n") return command({ action: "move", delta: 1 })
    if (key.ctrl && name === "p") return command({ action: "move", delta: -1 })
    if (key.ctrl || key.meta) {
      this.#count = undefined
      return { kind: "unhandled" }
    }

    if (!key.shift && /^[0-9]$/.test(name)) {
      if (name === "0" && this.#count === undefined) return { kind: "unhandled" }
      this.#count = Math.min(999, (this.#count ?? 0) * 10 + Number(name))
      return { kind: "pending" }
    }
    if (this.#count !== undefined && (name !== "g" || key.shift)) {
      this.#count = undefined
    }

    if (name === "f10") return { kind: "function", action: "quit" }
    if (name === "f3") return { kind: "function", action: "view" }
    if (name === "f4") return { kind: "function", action: "edit" }
    if (name === "f5") return { kind: "function", action: "copy" }
    if (name === "f6") return { kind: "function", action: "move" }
    if (name === "f7") return { kind: "function", action: "mkdir" }
    if (name === "f8") return { kind: "function", action: "delete" }
    if (name === "f9") return { kind: "function", action: "mount-ssh" }
    if (name === "z" && key.shift) {
      this.#prefix = "shift-z"
      return { kind: "pending" }
    }
    if (name === "g" && key.shift) return command({ action: "move-to", target: "last" })
    if (name === "home") return command({ action: "move-to", target: "first" })
    if (name === "end") return command({ action: "move-to", target: "last" })
    if (name === "p") return { kind: "function", action: key.shift ? "put-move" : "put-copy" }
    if (name === "d") {
      this.#prefix = "d"
      return { kind: "pending" }
    }
    if (name === "y" && key.shift) return { kind: "function", action: "yank" }
    if (name === "u") return command({ action: "undo" })
    if (name === "n") return command({ action: "search-next", direction: key.shift ? -1 : 1 })
    if (name === "/" || name === "slash") return { kind: "search", direction: 1 }
    if (name === "?" || name === "question") return { kind: "search", direction: -1 }
    if (key.shift) return { kind: "unhandled" }
    if (name === "q") {
      this.#prefix = "q"
      return { kind: "pending" }
    }
    if (name === "g") {
      this.#prefix = "g"
      return { kind: "pending" }
    }
    if (name === "y") {
      this.#prefix = "y"
      return { kind: "pending" }
    }
    if (name === "h" || name === "backspace") return command({ action: "parent" })
    if (name === "j" || name === "down") return command({ action: "move", delta: 1 })
    if (name === "k" || name === "up") return command({ action: "move", delta: -1 })
    if (name === "l" || name === "return") return command({ action: "enter" })
    if (name === "left") return command({ action: "parent" })
    if (name === "right") return command({ action: "enter" })
    if (name === "space") return { kind: "function", action: "quick-view" }
    if (name === "tab") return command({ action: "focus-next" })
    if (name === "t") return command({ action: "toggle-selection" })
    return { kind: "unhandled" }
  }
}
