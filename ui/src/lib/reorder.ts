// Pure helpers for drag-to-reorder of FX slots.

export interface SlotRect {
  left: number;
  top: number;
  width: number;
  height: number;
}

/**
 * Insertion index (0..rects.length) for a pointer at (x, y). Horizontal chains may wrap: a slot on an earlier
 * row (entirely above the pointer) always counts as "before"; on the pointer's row the horizontal midpoint decides.
 */
export function insertionIndex(rects: SlotRect[], x: number, y: number, vertical: boolean): number {
  let idx = 0;
  rects.forEach((r, i) => {
    const past = vertical
      ? y > r.top + r.height / 2
      : y > r.top + r.height || (y >= r.top - 4 && x > r.left + r.width / 2);
    if (past) idx = i + 1;
  });
  return idx;
}

/** Convert an insertion index into a `move_fx` target position (index after removal), or null if unchanged. */
export function moveTarget(from: number, insertAt: number): number | null {
  if (from < 0) return null;
  const to = insertAt > from ? insertAt - 1 : insertAt;
  return to === from ? null : to;
}
