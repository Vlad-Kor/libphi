import { build } from "esbuild";
import { copyFile, cp, mkdir, rm } from "node:fs/promises";
import { resolve } from "node:path";

const outdir = resolve(process.argv[2] ?? "dist");
await mkdir(outdir, { recursive: true });
await rm(resolve(outdir, "mathjax"), { recursive: true, force: true });
await rm(resolve(outdir, "mathjax-font"), { recursive: true, force: true });
await rm(resolve(outdir, "mathjax-mhchem-font-extension"), { recursive: true, force: true });
await rm(resolve(outdir, "chunks"), { recursive: true, force: true });
await mkdir(resolve(outdir, "mathjax"), { recursive: true });
await mkdir(resolve(outdir, "mathjax-mhchem-font-extension"), { recursive: true });

await build({
  entryPoints: ["src/main.ts"],
  bundle: true,
  format: "iife",
  target: ["safari17"],
  outfile: resolve(outdir, "editor.js"),
  minify: true,
  sourcemap: false,
  legalComments: "none",
  loader: { ".txt": "text" },
});

await build({
  entryPoints: ["src/mermaid-runtime.ts"],
  bundle: true,
  format: "iife",
  target: ["safari17"],
  outfile: resolve(outdir, "mermaid.js"),
  minify: true,
  sourcemap: false,
  legalComments: "none",
});

await copyFile("src/index.html", resolve(outdir, "index.html"));
await copyFile("src/styles/editor.css", resolve(outdir, "editor.css"));
await copyFile("src/math/mathjax-config.js", resolve(outdir, "mathjax-config.js"));
await copyFile("src/latex-suite/default-snippets.txt", resolve(outdir, "default-snippets.txt"));
await copyFile(
  "src/latex-suite/default-snippet-variables.txt",
  resolve(outdir, "default-snippet-variables.txt"),
);
await copyFile(
  "src/latex-suite/obsidian-latex-suite.LICENSE.md",
  resolve(outdir, "obsidian-latex-suite.LICENSE.md"),
);
await copyFile("node_modules/mathjax/tex-svg.js", resolve(outdir, "mathjax/tex-svg.js"));
await cp("node_modules/mathjax/input/tex/extensions", resolve(outdir, "mathjax/input/tex/extensions"), { recursive: true });
await cp("node_modules/mathjax/a11y", resolve(outdir, "mathjax/a11y"), { recursive: true });
await cp("node_modules/mathjax/sre", resolve(outdir, "mathjax/sre"), { recursive: true });
await cp("node_modules/@mathjax/mathjax-newcm-font/svg", resolve(outdir, "mathjax-font/svg"), { recursive: true });
await copyFile(
  "node_modules/@mathjax/mathjax-mhchem-font-extension/svg.js",
  resolve(outdir, "mathjax-mhchem-font-extension/svg.js"),
);
