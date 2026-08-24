import { readdir, readFile, stat, writeFile } from "node:fs/promises";
import { dirname, relative, resolve, sep } from "node:path";

const ALLOWED_LICENSES = new Set([
  "Apache-2.0",
  "BSD-2-Clause",
  "BSD-3-Clause",
  "ISC",
  "MIT",
  "MIT-0",
  "Unlicense",
]);

const LICENSE_OVERRIDES = new Map([
  ["dompurify", "Apache-2.0"],
  ["khroma", "MIT"],
]);

function repositoryUrl(repository) {
  const value = typeof repository === "string" ? repository : repository?.url;
  if (!value) return "Not declared";
  return value
    .replace(/^git\+/, "")
    .replace(/^git:\/\/github\.com\//, "https://github.com/")
    .replace(/\.git$/, "");
}

async function namedPackageAt(directory) {
  try {
    const packageJson = JSON.parse(
      await readFile(resolve(directory, "package.json"), "utf8"),
    );
    if (!packageJson.name || !packageJson.version) return null;
    return { root: directory, packageJson };
  } catch {
    return null;
  }
}

async function packageForInput(input) {
  let directory = dirname(resolve(input));
  const nodeModulesSegment = `${sep}node_modules${sep}`;
  if (!directory.includes(nodeModulesSegment)) return null;

  while (directory.includes(nodeModulesSegment)) {
    const found = await namedPackageAt(directory);
    if (found) return found;
    const parent = dirname(directory);
    if (parent === directory) break;
    directory = parent;
  }
  return null;
}

async function regularFiles(directory) {
  const entries = await readdir(directory, { withFileTypes: true });
  return entries.filter((entry) => entry.isFile()).map((entry) => entry.name);
}

function preferredLicenseFile(files) {
  const candidates = files.filter((name) =>
    /^(licen[sc]e|copying)(?:[-.].*)?$/i.test(name)
  );
  const preference = [
    "LICENSE",
    "LICENCE",
    "COPYING",
    "LICENSE.md",
    "LICENSE.txt",
    "LICENCE.md",
    "LICENCE.txt",
    "COPYING.md",
    "COPYING.txt",
    "license",
    "licence",
  ];
  for (const wanted of preference) {
    const match = candidates.find((name) => name === wanted);
    if (match) return match;
  }
  return candidates.sort((a, b) => a.localeCompare(b, "en"))[0] ?? null;
}

async function fastdomLicense(root) {
  const readme = await readFile(resolve(root, "README.md"), "utf8");
  const start = readme.search(/^## License\s*$/m);
  if (start < 0) return null;
  const remainder = readme.slice(start).replace(/^## License\s*\n/, "");
  const nextSection = remainder.search(/^##\s+/m);
  return (nextSection < 0 ? remainder : remainder.slice(0, nextSection)).trim();
}

async function packageLicense(component) {
  const { root, packageJson } = component;
  const files = await regularFiles(root);
  const licenseName = preferredLicenseFile(files);
  let licenseText = licenseName
    ? (await readFile(resolve(root, licenseName), "utf8")).trim()
    : null;
  let licenseSource = licenseName ?? null;

  if (!licenseText && packageJson.name === "fastdom") {
    licenseText = await fastdomLicense(root);
    licenseSource = "README.md#License";
  }
  if (!licenseText && packageJson.name.startsWith("@mathjax/")) {
    licenseText = (
      await readFile(resolve("node_modules/mathjax/LICENSE"), "utf8")
    ).trim();
    licenseSource = "mathjax/LICENSE (shared MathJax Apache-2.0 text)";
  }
  if (!licenseText) {
    throw new Error(
      `No distributable license text found for ${packageJson.name}@${packageJson.version}`,
    );
  }

  const declared = typeof packageJson.license === "object"
    ? packageJson.license?.type
    : packageJson.license;
  const selectedLicense = LICENSE_OVERRIDES.get(packageJson.name) ?? declared;
  if (!ALLOWED_LICENSES.has(selectedLicense)) {
    throw new Error(
      `Unreviewed license ${JSON.stringify(declared)} for ` +
        `${packageJson.name}@${packageJson.version}`,
    );
  }

  const noticeName = files.find((name) => /^NOTICE(?:\.[^.]+)?$/i.test(name));
  const noticeText = noticeName
    ? (await readFile(resolve(root, noticeName), "utf8")).trim()
    : null;
  return {
    name: packageJson.name,
    version: packageJson.version,
    declaredLicense: declared ?? "Not declared in package.json",
    selectedLicense,
    repository: repositoryUrl(packageJson.repository ?? packageJson.homepage),
    licenseSource,
    licenseText,
    noticeName,
    noticeText,
  };
}

async function additionalWork(work) {
  const path = resolve(work.licenseFile);
  if (!(await stat(path)).isFile()) {
    throw new Error(`Missing license file for ${work.name}: ${work.licenseFile}`);
  }
  if (!ALLOWED_LICENSES.has(work.declaredLicense)) {
    throw new Error(`Unreviewed license ${work.declaredLicense} for ${work.name}`);
  }
  return {
    ...work,
    selectedLicense: work.declaredLicense,
    licenseSource: relative(process.cwd(), path),
    licenseText: (await readFile(path, "utf8")).trim(),
    noticeName: null,
    noticeText: null,
  };
}

function renderComponent(component) {
  const declared = component.declaredLicense === component.selectedLicense
    ? component.declaredLicense
    : `${component.declaredLicense}; distributed under ${component.selectedLicense}`;
  const divider = "=".repeat(78);
  let result = [
    divider,
    `${component.name}@${component.version}`,
    `License: ${declared}`,
    `Source: ${component.repository}`,
    `License text from: ${component.licenseSource}`,
    divider,
    component.licenseText,
  ].join("\n");
  if (component.noticeText) {
    result += `\n\n${component.noticeName}\n${"-".repeat(78)}\n${component.noticeText}`;
  }
  return result;
}

export async function generateThirdPartyLicenses({
  metafiles,
  additionalPackageRoots = [],
  additionalWorks = [],
  outputFile,
}) {
  const packageRoots = new Set(additionalPackageRoots.map((root) => resolve(root)));
  for (const metafile of metafiles) {
    for (const input of Object.keys(metafile.inputs)) {
      const found = await packageForInput(input);
      if (found) packageRoots.add(found.root);
    }
  }

  const components = [];
  const keys = new Set();
  for (const root of [...packageRoots].sort((a, b) => a.localeCompare(b, "en"))) {
    const found = await namedPackageAt(root);
    if (!found) throw new Error(`No named package.json found at ${root}`);
    const key = `${found.packageJson.name}@${found.packageJson.version}`;
    if (keys.has(key)) continue;
    keys.add(key);
    components.push(await packageLicense(found));
  }
  for (const work of additionalWorks) {
    const component = await additionalWork(work);
    const key = `${component.name}@${component.version}`;
    if (keys.has(key)) continue;
    keys.add(key);
    components.push(component);
  }
  components.sort((a, b) =>
    `${a.name}@${a.version}`.localeCompare(`${b.name}@${b.version}`, "en")
  );

  const inventory = components.map((component) => {
    const license = component.declaredLicense === component.selectedLicense
      ? component.selectedLicense
      : `${component.selectedLicense} (selected from ${component.declaredLicense})`;
    return `- ${component.name}@${component.version} — ${license}`;
  });
  const reportHeader = [
    "PHI WEB EDITOR THIRD-PARTY LICENSES",
    "",
    "This file is generated from the actual production bundle inputs and the",
    "dependency metadata for copied upstream-built assets by",
    "web/editor/license-report.mjs. Do not edit it manually.",
    "The JavaScript artifacts are bundled and minified for Phi; they are not",
    "verbatim copies of the upstream source distributions.",
    "",
    "Component inventory",
    "-------------------",
    ...inventory,
    "",
    "Full license and notice texts",
    "-----------------------------",
  ].join("\n");
  const report = `${reportHeader}\n\n${components.map(renderComponent).join("\n\n")}\n`;
  await writeFile(outputFile, report, "utf8");
}
