import { build } from "esbuild";
import { copyFile, cp, mkdir, rm } from "node:fs/promises";
import { resolve } from "node:path";
import { generateThirdPartyLicenses } from "./license-report.mjs";

const outdir = resolve(process.argv[2] ?? "dist");
await mkdir(outdir, { recursive: true });
await rm(resolve(outdir, "mathjax"), { recursive: true, force: true });
await rm(resolve(outdir, "mathjax-font"), { recursive: true, force: true });
await rm(resolve(outdir, "mathjax-dsfont-font-extension"), { recursive: true, force: true });
await rm(resolve(outdir, "mathjax-mhchem-font-extension"), { recursive: true, force: true });
await rm(resolve(outdir, "chunks"), { recursive: true, force: true });
await mkdir(resolve(outdir, "mathjax"), { recursive: true });
await mkdir(resolve(outdir, "mathjax-dsfont-font-extension"), { recursive: true });
await mkdir(resolve(outdir, "mathjax-mhchem-font-extension"), { recursive: true });

const legalBanner = "/*! Third-party licenses: THIRD_PARTY_LICENSES.txt */";

const editorBuild = await build({
  entryPoints: ["src/main.ts"],
  bundle: true,
  format: "iife",
  target: ["safari17"],
  outfile: resolve(outdir, "editor.js"),
  minify: true,
  sourcemap: false,
  legalComments: "none",
  banner: { js: legalBanner },
  metafile: true,
  loader: { ".txt": "text" },
});

const mermaidBuild = await build({
  entryPoints: ["src/mermaid-runtime.ts"],
  bundle: true,
  format: "iife",
  target: ["safari17"],
  outfile: resolve(outdir, "mermaid.js"),
  minify: true,
  sourcemap: false,
  legalComments: "none",
  banner: { js: legalBanner },
  metafile: true,
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
await copyFile(
  "src/latex-suite/vimtex.LICENSE.md",
  resolve(outdir, "vimtex.LICENSE.md"),
);
await copyFile("node_modules/mathjax/tex-svg.js", resolve(outdir, "mathjax/tex-svg.js"));
await cp("node_modules/mathjax/input/tex/extensions", resolve(outdir, "mathjax/input/tex/extensions"), { recursive: true });
await cp("node_modules/mathjax/a11y", resolve(outdir, "mathjax/a11y"), { recursive: true });
await cp("node_modules/mathjax/sre", resolve(outdir, "mathjax/sre"), { recursive: true });
await cp("node_modules/@mathjax/mathjax-newcm-font/svg", resolve(outdir, "mathjax-font/svg"), { recursive: true });
await copyFile(
  "node_modules/@mathjax/mathjax-dsfont-font-extension/svg.js",
  resolve(outdir, "mathjax-dsfont-font-extension/svg.js"),
);
await copyFile(
  "node_modules/@mathjax/mathjax-mhchem-font-extension/svg.js",
  resolve(outdir, "mathjax-mhchem-font-extension/svg.js"),
);

await generateThirdPartyLicenses({
  metafiles: [editorBuild.metafile, mermaidBuild.metafile],
  additionalPackageRoots: [
    // MathJax publishes some files as upstream-built bundles. Keep the exact
    // source package and its bundled runtime components in the inventory too.
    "node_modules/@mathjax/src",
    "node_modules/mathjax",
    "node_modules/@mathjax/mathjax-newcm-font",
    "node_modules/@mathjax/mathjax-dsfont-font-extension",
    "node_modules/@mathjax/mathjax-mhchem-font-extension",
    "node_modules/mhchemparser",
    "node_modules/mj-context-menu",
    "node_modules/speech-rule-engine",
    "node_modules/@xmldom/xmldom",
    "node_modules/speech-rule-engine/node_modules/commander",
    "node_modules/wicked-good-xpath",
  ],
  additionalWorks: [
    {
      name: "Obsidian LaTeX Suite-derived snippet defaults and conceal mappings",
      version: "source-derived",
      declaredLicense: "MIT",
      repository: "https://github.com/artisticat1/obsidian-latex-suite",
      licenseFile: "src/latex-suite/obsidian-latex-suite.LICENSE.md",
    },
    {
      name: "VimTeX-derived conceal mappings",
      version: "source-derived",
      declaredLicense: "MIT",
      repository: "https://github.com/lervag/vimtex",
      licenseFile: "src/latex-suite/vimtex.LICENSE.md",
    },
  ],
  outputFile: resolve(outdir, "THIRD_PARTY_LICENSES.txt"),
});
