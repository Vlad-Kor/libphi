import { describe, expect, it } from "vitest";
import { exportScale, noteTitle } from "../src/export-utils";

describe("PDF export helpers", () => {
  it("derives section titles from every supported note extension", () => {
    expect(noteTitle("Notes/First.md")).toBe("First");
    expect(noteTitle("Notes/Second.markdown")).toBe("Second");
    expect(noteTitle("Notes/Third.TXT")).toBe("Third");
  });

  it("keeps dots that are part of the note name", () => {
    expect(noteTitle("Notes/release.v2.md")).toBe("release.v2");
  });

  it("normalizes and clamps whole-document scale percentages", () => {
    expect(exportScale(50)).toBe(0.5);
    expect(exportScale(125)).toBe(1.25);
    expect(exportScale(200)).toBe(2);
    expect(exportScale(20)).toBe(0.5);
    expect(exportScale(Number.NaN)).toBe(1);
  });
});
