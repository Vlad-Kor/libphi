// @vitest-environment jsdom
import { EditorState } from "@codemirror/state";
import { EditorView, WidgetType } from "@codemirror/view";
import { describe, expect, it, vi } from "vitest";
import { PreviewIdleGate } from "../src/markdown/preview-idle";
import { MermaidWidget, LinkWidget, MarkdownLinkWidget, setPreviewGeometryContext, seedPreviewImageGeometry, resetPreviewGeometryCaches } from "../src/widgets/preview";
import { clearPreviewGeometry, measuredPreviewGeometry, previewGeometry, settlePreview, withMeasuredGeometry } from "../src/markdown/geometry";

class Preview extends WidgetType {
  get estimatedHeight() { return 64; }
  eq(other: Preview) { return other instanceof Preview; }
  toDOM() { return document.createElement('div'); }
}

describe('background preview geometry', () => {
  it('does not replace intrinsic header dimensions with a resized SVG instance', () => {
    setPreviewGeometryContext('/geometry-image.md', 780, 1);
    seedPreviewImageGeometry([{ target: 'image.svg', path: 'image.svg', width: 300, height: 150 }]);
    try {
      const small = new LinkWidget('image.svg|230', '230', 0, 18, true, true);
      const dom = small.toDOM({ requestMeasure() {} } as unknown as EditorView);
      const image = dom.querySelector('img')!;
      Object.defineProperties(image, { naturalWidth: { value: 230 }, naturalHeight: { value: 115 } });
      image.dispatchEvent(new Event('load'));
      const full = new MarkdownLinkWidget('image.svg', 'local', 20, true, 40, true);
      expect(full.estimatedHeight).toBe(150);
      expect(full.toDOM({ requestMeasure() {} } as unknown as EditorView).querySelector('img')!.width).toBe(300);
    } finally {
      resetPreviewGeometryCaches();
      setPreviewGeometryContext('', 780, 1);
    }
  });
  it('defers a completed batch until scrolling has actually been quiet, and cancels on document replacement', async () => {
    vi.useFakeTimers();
    try {
      const gate = new PreviewIdleGate();
      const publish = vi.fn();
      const signal = new AbortController();
      gate.noteActivity();
      const pending = gate.wait(signal.signal).then(ready => { if (ready) publish(); });
      for (let i = 0; i < 10; i++) {
        await vi.advanceTimersByTimeAsync(100);
        gate.noteActivity();
      }
      expect(publish).not.toHaveBeenCalled();
      await vi.advanceTimersByTimeAsync(149);
      expect(publish).not.toHaveBeenCalled();
      await vi.advanceTimersByTimeAsync(1);
      await pending;
      expect(publish).toHaveBeenCalledOnce();
      gate.noteActivity();
      const cancelled = gate.wait(signal.signal);
      signal.abort();
      expect(await cancelled).toBe(false);
      expect(vi.getTimerCount()).toBe(0);
    } finally {
      vi.useRealTimers();
    }
  });
  it('reuses Mermaid output with distinct SVG and accessibility references per mount', async () => {
    const render = vi.fn(async () => ({ svg: '<svg id="diagram" aria-labelledby="label"><title id="label">Diagram</title><defs><marker id="arrow"/></defs><path marker-end="url(#arrow)"/></svg>' }));
    const runtime = window as unknown as { phiMermaid?: unknown };
    runtime.phiMermaid = { render };
    try {
      const widget = new MermaidWidget('graph TD; GeometryA --> GeometryB', 0);
      const first = widget.toDOM({} as EditorView);
      await vi.waitFor(() => expect(first.querySelector('svg')).not.toBeNull());
      const second = widget.toDOM({} as EditorView);
      expect(render).toHaveBeenCalledTimes(1);
      expect(second.querySelector('svg')!.id).not.toBe(first.querySelector('svg')!.id);
      expect(second.querySelector('svg')!.getAttribute('aria-labelledby')).toBe(second.querySelector('title')!.id);
      expect(second.querySelector('path')!.getAttribute('marker-end')).toBe(`url(#${second.querySelector('marker')!.id})`);
    } finally {
      delete runtime.phiMermaid;
    }
  });
  it('invalidates widget equality in both directions when a measured height arrives', () => {
    const cold = new Preview(), measured = new Preview(), same = new Preview();
    withMeasuredGeometry(cold, 'math', new Map());
    withMeasuredGeometry(measured, 'math', new Map([['math', 42.123456]]));
    withMeasuredGeometry(same, 'math', new Map([['math', 42.123456]]));
    expect(cold.eq(measured)).toBe(false);
    expect(measured.eq(cold)).toBe(false);
    expect(measured.eq(same)).toBe(true);
    expect(measured.estimatedHeight).toBe(42.123456);
    expect(measured).toBeInstanceOf(Preview);
  });

  it('keeps every height in the active document and clears on edits and geometry changes', () => {
    let state = EditorState.create({ doc: 'abc', extensions: [previewGeometry] });
    const heights = new Map(Array.from({ length: 1024 }, (_, i) => [String(i), i + 0.125]));
    state = state.update({ effects: measuredPreviewGeometry.of(heights) }).state;
    expect(state.field(previewGeometry).size).toBe(1024);
    state = state.update({ selection: { anchor: 2 } }).state;
    expect(state.field(previewGeometry).size).toBe(1024);
    state = state.update({ changes: { from: 1, insert: 'x' } }).state;
    expect(state.field(previewGeometry).size).toBe(0);
    state = state.update({ effects: measuredPreviewGeometry.of(heights) }).state;
    state = state.update({ effects: clearPreviewGeometry.of(null) }).state;
    expect(state.field(previewGeometry).size).toBe(0);
  });

  it('cancels unresolved renderers immediately instead of publishing a placeholder height', async () => {
    const root = document.createElement('div');
    root.innerHTML = '<span class="math-loading">x</span>';
    const abort = new AbortController();
    const ready = settlePreview(root, abort.signal);
    abort.abort();
    expect(await ready).toBe(false);
  });
});
