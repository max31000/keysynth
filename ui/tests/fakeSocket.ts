import type { WebSocketLike } from '../src/protocol/client';

/** In-memory WebSocket double. */
export class FakeSocket implements WebSocketLike {
  static all: FakeSocket[] = [];
  readyState = 0;
  sent: Record<string, unknown>[] = [];
  onopen: ((ev: unknown) => void) | null = null;
  onclose: ((ev: unknown) => void) | null = null;
  onerror: ((ev: unknown) => void) | null = null;
  onmessage: ((ev: { data: unknown }) => void) | null = null;

  constructor(public url: string) {
    FakeSocket.all.push(this);
  }
  send(data: string) {
    this.sent.push(JSON.parse(data));
  }
  close() {
    this.readyState = 3;
  }
  // test helpers
  open() {
    this.readyState = 1;
    this.onopen?.({});
  }
  drop() {
    this.readyState = 3;
    this.onclose?.({});
  }
  receive(msg: object) {
    this.onmessage?.({ data: JSON.stringify(msg) });
  }
  last() {
    return this.sent[this.sent.length - 1]!;
  }
}

export const latestSocket = () => FakeSocket.all[FakeSocket.all.length - 1]!;
