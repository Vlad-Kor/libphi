import type { NativeMessage } from "./types";

type ResponseHandler = {
  resolve: (value: unknown) => void;
  reject: (reason?: unknown) => void;
  timer: number;
};

const pending = new Map<string, ResponseHandler>();
let sequence = 0;

export function sendNative(
  type: string,
  payload: Record<string, unknown> = {},
  id?: string,
): void {
  const message: NativeMessage = { protocol: 1, type, payload };
  if (id) message.id = id;
  const handler = window.webkit?.messageHandlers?.native;
  if (handler) handler.postMessage(JSON.stringify(message));
  else window.dispatchEvent(new CustomEvent("phi-native-message", { detail: message }));
}

export function requestNative<T = unknown>(
  type: string,
  payload: Record<string, unknown> = {},
  timeout = 5000,
): Promise<T> {
  const id = `web-${Date.now()}-${++sequence}`;
  return new Promise<T>((resolve, reject) => {
    const timer = window.setTimeout(() => {
      pending.delete(id);
      reject(new Error(`Native request timed out: ${type}`));
    }, timeout);
    pending.set(id, { resolve: resolve as (value: unknown) => void, reject, timer });
    sendNative(type, payload, id);
  });
}

export function acceptNativeResponse(message: NativeMessage): boolean {
  if (message.type !== "request/response" || !message.id) return false;
  const handler = pending.get(message.id);
  if (!handler) return false;
  pending.delete(message.id);
  window.clearTimeout(handler.timer);
  if (message.payload?.error) handler.reject(new Error(String(message.payload.error)));
  else handler.resolve(message.payload?.result);
  return true;
}

export function reportError(
  error: unknown,
  component: string,
  documentPath = "",
): void {
  const value = error instanceof Error ? error : new Error(String(error));
  sendNative("log/error", {
    message: value.message,
    stack: value.stack ?? "",
    documentPath,
    component,
  });
}
