import { CompletionContext } from "@codemirror/autocomplete";
import { EditorState } from "@codemirror/state";
import { afterEach, describe, expect, it, vi } from "vitest";

const bridge = vi.hoisted(() => ({ requestNative: vi.fn() }));
vi.mock("../src/bridge", () => bridge);

import { markdownCompletion, NATIVE_COMPLETION_LIMIT } from "../src/markdown/completion";

function contextAtEnd(text: string): CompletionContext {
  const state = EditorState.create({ doc: text });
  return new CompletionContext(state, text.length, false);
}

describe("wiki link completion", () => {
  afterEach(() => bridge.requestNative.mockReset());

  it("filters a complete native result locally", async () => {
    bridge.requestNative.mockResolvedValue(["Alpha.md", "folder/Alps.md"]);
    const result = await markdownCompletion(contextAtEnd("See [[al"));
    expect(bridge.requestNative).toHaveBeenCalledWith(
      "completion/files", { query: "al", target: "" },
    );
    expect(result?.options.map((option) => option.label)).toEqual(["Alpha", "Alps"]);
    expect(result?.validFor).toBeDefined();
  });

  it("asks again when the native result was truncated", async () => {
    bridge.requestNative.mockResolvedValue(
      Array.from({ length: NATIVE_COMPLETION_LIMIT }, (_, i) => `note ${i}.md`),
    );
    const result = await markdownCompletion(contextAtEnd("See [[no"));
    expect(result?.options).toHaveLength(NATIVE_COMPLETION_LIMIT);
    expect(result?.validFor).toBeUndefined();
  });
});
