type CalloutIcon = "note" | "info" | "todo" | "tip" | "success" |
  "question" | "warning" | "error" | "bug" | "example" | "quote";

const iconPaths: Record<CalloutIcon, string[]> = {
  note: ["M3.5 2.5h6l3 3v8h-9z", "M9.5 2.5v3h3", "M5.5 8h5M5.5 10.5h5"],
  info: ["M8 14a6 6 0 1 0 0-12 6 6 0 0 0 0 12Z", "M8 7v4M8 4.75v.5"],
  todo: ["M3 3h10v10H3z", "m5 8 1.5 1.5L10 6"],
  tip: ["M5.25 10.5h5.5M6 12.5h4", "M5.2 8.7A4.25 4.25 0 1 1 10.8 8.7c-.75.55-1.05 1.05-1.05 1.8h-3.5c0-.75-.3-1.25-1.05-1.8Z"],
  success: ["M8 14a6 6 0 1 0 0-12 6 6 0 0 0 0 12Z", "m5 8 2 2 4-4"],
  question: ["M8 14a6 6 0 1 0 0-12 6 6 0 0 0 0 12Z", "M6.4 6a1.7 1.7 0 0 1 3.2.8c0 1.2-1.6 1.45-1.6 2.45M8 11.5v.25"],
  warning: ["M8 2.25 14 13H2z", "M8 6v3.25M8 11.25v.25"],
  error: ["M8 14a6 6 0 1 0 0-12 6 6 0 0 0 0 12Z", "m5.75 5.75 4.5 4.5M10.25 5.75l-4.5 4.5"],
  bug: ["M5 6h6v4.25a3 3 0 0 1-6 0z", "M6.25 6V4.75a1.75 1.75 0 0 1 3.5 0V6M3 7h2M11 7h2M3 10h2M11 10h2"],
  example: ["M6 2.5h4M7 2.5v3l-3 6.25c-.4.8.1 1.75 1 1.75h6.2c.9 0 1.4-.95 1-1.75L9 5.5v-3", "M5.5 10h5"],
  quote: ["M3.25 5.25h3.5v3.5h-2c0 1-.5 1.75-1.5 2.25M9.25 5.25h3.5v3.5h-2c0 1-.5 1.75-1.5 2.25"],
};

function iconForType(type: string): CalloutIcon {
  const normalized = type.toLowerCase();
  if (["abstract", "summary", "tldr"].includes(normalized)) return "note";
  if (["info"].includes(normalized)) return "info";
  if (["todo"].includes(normalized)) return "todo";
  if (["tip", "hint", "important"].includes(normalized)) return "tip";
  if (["success", "check", "done"].includes(normalized)) return "success";
  if (["question", "help", "faq"].includes(normalized)) return "question";
  if (["warning", "caution", "attention"].includes(normalized)) return "warning";
  if (["failure", "fail", "missing", "danger", "error"].includes(normalized)) return "error";
  if (normalized === "bug") return "bug";
  if (normalized === "example") return "example";
  if (["quote", "cite"].includes(normalized)) return "quote";
  return "note";
}

export function calloutIcon(type: string): SVGSVGElement {
  const name = iconForType(type);
  const svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
  svg.classList.add("callout-icon");
  svg.dataset.calloutIcon = name;
  svg.setAttribute("viewBox", "0 0 16 16");
  svg.setAttribute("aria-hidden", "true");
  svg.setAttribute("fill", "none");
  svg.setAttribute("stroke", "currentColor");
  svg.setAttribute("stroke-width", "1.4");
  svg.setAttribute("stroke-linecap", "round");
  svg.setAttribute("stroke-linejoin", "round");
  for (const data of iconPaths[name]) {
    const path = document.createElementNS("http://www.w3.org/2000/svg", "path");
    path.setAttribute("d", data);
    svg.append(path);
  }
  return svg;
}
