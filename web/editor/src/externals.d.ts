declare module "*.txt" {
  const value: string;
  export default value;
}

declare module "markdown-it-footnote" {
  const plugin: (markdown: unknown) => void;
  export default plugin;
}

declare module "markdown-it-task-lists" {
  const plugin: (markdown: unknown, options?: { enabled?: boolean; label?: boolean; labelAfter?: boolean }) => void;
  export default plugin;
}

declare module "mathjax/tex-svg.js";
