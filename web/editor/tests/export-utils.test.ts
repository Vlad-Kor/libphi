import { describe, expect, it } from "vitest";
import { noteTitle } from "../src/export-utils";

describe("PDF export helpers", () => {
  it("derives section titles from every supported note extension", () => {
    expect(noteTitle("Notes/First.md")).toBe("First");
    expect(noteTitle("Notes/Second.markdown")).toBe("Second");
    expect(noteTitle("Notes/Third.TXT")).toBe("Third");
  });

  it("keeps dots that are part of the note name", () => {
    expect(noteTitle("Notes/release.v2.md")).toBe("release.v2");
  });
});
