import solidPlugin from "@opentui/solid/bun-plugin"

import { resolve } from "node:path"

const commit = process.env.NEOVIFM_BUILD_COMMIT?.trim()
const outfile = process.env.NEOVIFM_PREVIEW_OUTFILE?.trim()

if (commit === undefined || !/^[0-9a-f]{40}$/.test(commit)) {
  throw new Error("NEOVIFM_BUILD_COMMIT must be a lowercase 40-character Git commit")
}
if (outfile === undefined || outfile.length === 0 || outfile.includes("\0")) {
  throw new Error("NEOVIFM_PREVIEW_OUTFILE must name the output executable")
}

const result = await Bun.build({
  entrypoints: [resolve(import.meta.dir, "../src/index.tsx")],
  plugins: [solidPlugin],
  define: {
    __NEOVIFM_BUILD_COMMIT__: JSON.stringify(commit),
    __NEOVIFM_STANDALONE__: "true",
  },
  compile: {
    outfile,
    autoloadDotenv: false,
    autoloadBunfig: false,
  },
})

if (!result.success) {
  for (const log of result.logs) console.error(log)
  process.exitCode = 1
}
