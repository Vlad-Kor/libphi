import JSON5 from "json5";

export interface LatexSnippet {
  trigger: string;
  replacement: string;
  options: string;
  priority: number;
  handler?: LatexSnippetHandler;
  description?: string;
  flags?: string;
  triggerKey?: string;
  language?: string;
  excludedMacros?: string[];
  excludedEnvironments?: string[];
  includedMacros?: string[];
}

export type LatexSnippetHandler =
  "space-after-symbol" |
  "protect-macro-prefix" |
  "identity-matrix" |
  "display-math-list";

const snippetHandlers = new Set<LatexSnippetHandler>([
  "space-after-symbol",
  "protect-macro-prefix",
  "identity-matrix",
  "display-math-list",
]);

export function parseSnippetFile(source: string): LatexSnippet[] {
  const parsed = JSON5.parse(source) as unknown;
  if (!Array.isArray(parsed)) throw new Error("Snippet file must contain an array");
  return parsed
    .filter((entry): entry is Record<string, unknown> => Boolean(entry) && typeof entry === "object")
    .filter((entry) => typeof entry.trigger === "string" && typeof entry.replacement === "string")
    .map((entry) => ({
      trigger: String(entry.trigger),
      replacement: String(entry.replacement),
      options: typeof entry.options === "string" ? entry.options : "m",
      priority: typeof entry.priority === "number" ? entry.priority : 0,
      handler: typeof entry.handler === "string" &&
        snippetHandlers.has(entry.handler as LatexSnippetHandler)
        ? entry.handler as LatexSnippetHandler : undefined,
      description: typeof entry.description === "string" ? entry.description : undefined,
      flags: typeof entry.flags === "string" ? entry.flags : undefined,
      triggerKey: typeof entry.triggerKey === "string" ? entry.triggerKey : undefined,
      language: typeof entry.language === "string" ? entry.language : undefined,
      excludedMacros: Array.isArray(entry.excludedMacros) ? entry.excludedMacros.map(String) : undefined,
      excludedEnvironments: Array.isArray(entry.excludedEnvironments) ? entry.excludedEnvironments.map(String) : undefined,
      includedMacros: Array.isArray(entry.includedMacros) ? entry.includedMacros.map(String) : undefined,
    }))
    .sort((left, right) => right.priority - left.priority);
}

export function parseSnippetVariables(source: string): Record<string, string> {
  const parsed = JSON5.parse(source) as unknown;
  if (!parsed || typeof parsed !== "object" || Array.isArray(parsed))
    throw new Error("Snippet variables must contain an object");
  const variables: Record<string, string> = {};
  for (const [rawName, value] of Object.entries(parsed)) {
    if (typeof value !== "string") continue;
    const wrapped = /^\$\{([^}]+)\}$/.exec(rawName);
    const name = wrapped?.[1] ?? rawName;
    if (/^[A-Z][A-Z0-9_]*$/.test(name)) variables[name] = value;
  }
  return variables;
}

export interface ExpandedSnippet {
  text: string;
  tabstops: { index: number; from: number; to: number }[];
}

export function expandReplacement(
  replacement: string,
  captures: string[] = [],
  visual = "",
): ExpandedSnippet {
  let source = replacement.replace(/\[\[(\d+)\]\]/g, (_match, index: string) => captures[Number(index)] ?? "");
  source = source.replace(/\$\{VISUAL\}/g, visual);
  const tabstops: ExpandedSnippet["tabstops"] = [];
  let text = "";
  let cursor = 0;
  const pattern = /\$\{(\d+):([^}]*)\}|\$(\d+)/g;
  for (const match of source.matchAll(pattern)) {
    text += source.slice(cursor, match.index);
    const index = Number(match[1] ?? match[3]);
    const value = match[2] ?? "";
    const from = text.length;
    text += value;
    tabstops.push({ index, from, to: text.length });
    cursor = match.index + match[0].length;
  }
  text += source.slice(cursor);
  tabstops.sort((left, right) => left.index - right.index || left.from - right.from);
  return { text, tabstops };
}
