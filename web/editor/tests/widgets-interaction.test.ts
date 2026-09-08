// @vitest-environment jsdom
import { EditorState } from "@codemirror/state";
import type { EditorView } from "@codemirror/view";
import { afterEach, expect, it, vi } from "vitest";
import { acceptNativeResponse } from "../src/bridge";
import type { NativeMessage } from "../src/types";
import { CalloutWidget, LinkWidget, RawHtmlWidget, resetPreviewGeometryCaches } from "../src/widgets/preview";

afterEach(() => {
  document.body.replaceChildren();
  resetPreviewGeometryCaches();
});

function mockView(doc = " ".repeat(200)) {
  return {
    state: EditorState.create({ doc }),
    dispatch: vi.fn(), focus: vi.fn(), requestMeasure: vi.fn(),
  } as unknown as EditorView;
}

it.each([false, true])("lets HTML summary close and reopen (initial open: %s)", open => {
  const view = mockView();
  const widget = new RawHtmlWidget(`<details${open ? " open" : ""}><summary><b>Heading</b></summary>Body</details>`, 0);
  const dom = widget.toDOM(view);
  document.body.append(dom);
  const details = dom.querySelector("details")!;
  const title = dom.querySelector("b")!;
  const down = new MouseEvent("pointerdown", { bubbles: true, cancelable: true });
  title.dispatchEvent(down);
  expect(down.defaultPrevented).toBe(false);
  title.click();
  expect(details.open).toBe(!open);
  title.click();
  expect(details.open).toBe(open);
  expect(view.dispatch).not.toHaveBeenCalled();
  details.dispatchEvent(new Event("toggle"));
  expect(view.requestMeasure).toHaveBeenCalled();
});

it("lets the callout button repeatedly expand and collapse without editing", () => {
  const view = mockView();
  const dom = new CalloutWidget("Body", "note", "Heading", "-", 0).toDOM(view) as HTMLDetailsElement;
  document.body.append(dom);
  const button = dom.querySelector("button")!;
  expect(button.getAttribute("aria-expanded")).toBe("false");
  expect(button.textContent).toBe("›");
  expect(button.closest(".callout-label")?.firstChild?.textContent).toBe("Heading");
  button.dispatchEvent(new MouseEvent("pointerdown", { bubbles: true, cancelable: true }));
  button.click();
  expect(dom.open).toBe(true);
  expect(button.getAttribute("aria-expanded")).toBe("true");
  expect(button.textContent).toBe("›");
  expect(button.getAttribute("aria-label")).toBe("Collapse callout");
  // Native keyboard activation also dispatches click, with no pointerdown.
  button.click();
  expect(dom.open).toBe(false);
  expect(button.getAttribute("aria-label")).toBe("Expand callout");
  expect(view.dispatch).not.toHaveBeenCalled();
  dom.dispatchEvent(new Event("toggle"));
  expect(view.requestMeasure).toHaveBeenCalled();
});

it.each(["pointerdown", "click"])("reveals a folded title-only callout on %s", eventType => {
  const source = "> [!tip]- Satz";
  const view = mockView(source);
  const dom = new CalloutWidget("", "tip", "Satz", "-", 0).toDOM(view) as HTMLDetailsElement;
  const title = dom.querySelector("summary")!;
  title.dispatchEvent(new MouseEvent(eventType, { bubbles: true, cancelable: true }));
  expect(view.dispatch).toHaveBeenCalledWith({
    selection: { anchor: source.indexOf("Satz") }, scrollIntoView: true,
  });
  expect(dom.open).toBe(false);
  expect(view.focus).toHaveBeenCalled();
});

it("reveals callout body source at its quoted content", () => {
  const source = "> [!note]+ Heading\n> Body";
  const view = mockView(source);
  const dom = new CalloutWidget("Body", "note", "Heading", "+", 0).toDOM(view) as HTMLDetailsElement;
  dom.querySelector(".callout-content")!.dispatchEvent(
    new MouseEvent("pointerdown", { bubbles: true, cancelable: true }));
  expect(view.dispatch).toHaveBeenCalledWith({
    selection: { anchor: source.indexOf("Body") }, scrollIntoView: true,
  });
  expect(dom.open).toBe(true);
});

it("keeps a title-only callout padding click out of the following paragraph", () => {
  const source = "> [!tip]- Satz\n\nFollowing paragraph";
  const view = mockView(source);
  const dom = new CalloutWidget("", "tip", "Satz", "-", 0).toDOM(view);
  dom.dispatchEvent(new MouseEvent("pointerdown", { bubbles: true, cancelable: true }));
  expect(view.dispatch).toHaveBeenCalledWith({
    selection: { anchor: source.indexOf("Satz") }, scrollIntoView: true,
  });
});

it("shares pending missing embeds and ignores replies to destroyed widgets", async () => {
  const requests: NativeMessage[] = [];
  const listener = (event: Event) => requests.push((event as CustomEvent<NativeMessage>).detail);
  window.addEventListener("phi-native-message", listener);
  const view = mockView();
  const first = new LinkWidget("Missing note", "Missing note", 0, 17, true);
  const second = new LinkWidget("Missing note", "Missing note", 20, 37, true);
  const firstDOM = first.toDOM(view);
  const secondDOM = second.toDOM(view);
  document.body.append(firstDOM, secondDOM);
  first.destroy(firstDOM);
  firstDOM.remove();
  expect(requests.filter(request => request.type === "embed/read")).toHaveLength(1);
  acceptNativeResponse({ protocol: 1, type: "request/response", id: requests[0].id,
    payload: { error: "Note was not found" } });
  await Promise.resolve();
  await Promise.resolve();
  expect(firstDOM.textContent).toContain("Loading embed");
  expect(secondDOM.querySelector(".render-error")?.textContent).toBe("Note was not found");
  expect(secondDOM.querySelector(".dimmed")).toBeNull();
  expect(secondDOM.style.minHeight).toBe("");
  expect(view.requestMeasure).toHaveBeenCalledTimes(1);
  second.destroy(secondDOM);
  window.removeEventListener("phi-native-message", listener);
});
