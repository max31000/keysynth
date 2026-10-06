import { useEffect, useRef } from 'react';
import { dbToMeter } from '../lib/paramMath';
import { live, onFrame } from '../store/liveBus';
import type { StereoPeak } from '../protocol/types';

export type MeterSource = () => StereoPeak | undefined;

export const masterSource: MeterSource = () => live.telemetry?.meters.master;
export const layerSource =
  (node: number | undefined): MeterSource =>
  () =>
    node === undefined ? undefined : live.telemetry?.meters.layers[String(node)];

const STALE_MS = 500;
const SCALE = [6, 0, -6, -12, -24, -36, -48];

/**
 * Stereo peak meter. Animated on the shared rAF loop with CSS transforms; ballistic fall + peak hold,
 * so 30 Hz telemetry looks smooth at 60 fps without React re-renders.
 */
export function StereoMeter({
  source,
  orientation = 'vertical',
  scale = false,
  className,
}: {
  source: MeterSource;
  orientation?: 'vertical' | 'horizontal';
  scale?: boolean;
  className?: string;
}) {
  const bars = useRef<(HTMLDivElement | null)[]>([]);
  const peaks = useRef<(HTMLDivElement | null)[]>([]);
  const clip = useRef<HTMLDivElement | null>(null);
  const srcRef = useRef(source);
  srcRef.current = source;

  useEffect(() => {
    const level = [0, 0];
    const peak = [0, 0];
    const hold = [0, 0];
    let clipUntil = 0;
    const axis = orientation === 'vertical' ? 'scaleY' : 'scaleX';
    const tr = orientation === 'vertical' ? 'translateY' : 'translateX';
    return onFrame((now, dt) => {
      const fresh = now - live.telemetryAt < STALE_MS;
      const v = fresh ? srcRef.current() : undefined;
      for (let ch = 0; ch < 2; ch++) {
        const db = v ? v[ch]! : -120;
        const target = dbToMeter(db);
        // instant attack, 20 dB/s fall: the meter spans 66 dB (-60..+6) per unit, so 20/66 units per second
        const fall = (dt / 1000) * (20 / 66);
        level[ch] = target > level[ch]! ? target : Math.max(target, level[ch]! - fall);
        if (level[ch]! >= peak[ch]!) {
          peak[ch] = level[ch]!;
          hold[ch] = now + 900;
        } else if (now > hold[ch]!) {
          peak[ch] = Math.max(level[ch]!, peak[ch]! - fall * 1.5);
        }
        if (db >= -0.1) clipUntil = now + 1500;
        const b = bars.current[ch];
        if (b) b.style.transform = `${axis}(${level[ch]!.toFixed(4)})`;
        const p = peaks.current[ch];
        if (p) {
          const pct = (orientation === 'vertical' ? -1 : 1) * peak[ch]! * 100;
          p.style.transform = `${tr}(${pct.toFixed(2)}%)`;
          p.style.opacity = peak[ch]! > 0.01 ? '1' : '0';
        }
      }
      if (clip.current) clip.current.classList.toggle('on', now < clipUntil);
    });
  }, [orientation]);

  return (
    <div className={`meter meter-${orientation} ${className ?? ''}`} aria-hidden>
      {orientation === 'vertical' && <div ref={clip} className="meter-clip" />}
      <div className="meter-chans">
        {[0, 1].map((ch) => (
          <div key={ch} className="meter-chan">
            <div className="meter-fill" ref={(el) => void (bars.current[ch] = el)} />
            <div className="meter-zero" />
            <div className="meter-peak-track" ref={(el) => void (peaks.current[ch] = el)}>
              <div className="meter-peak" />
            </div>
          </div>
        ))}
      </div>
      {scale && orientation === 'vertical' && (
        <div className="meter-scale">
          {SCALE.map((db) => (
            <span key={db} style={{ bottom: `${dbToMeter(db) * 100}%` }}>
              {db > 0 ? `+${db}` : db}
            </span>
          ))}
        </div>
      )}
    </div>
  );
}
