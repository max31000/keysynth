import { memo } from 'react';
import { actions, useEngine } from '../store';
import { findLayer, isAudible } from '../store/patchOps';
import type { Layer, ParamSpec } from '../protocol/types';
import { Knob } from './Knob';
import { StereoMeter, layerSource } from './Meter';
import { ZoneEditor } from './ZoneEditor';
import { FxChain } from './FxChain';
import { ModulePanel } from './ModulePanel';

const spec = (id: string, name: string, min: number, max: number, def: number, unit = '', scale: ParamSpec['scale'] = 'linear'): ParamSpec => ({
  id,
  name,
  group: 'Zone',
  unit,
  min,
  max,
  def,
  scale,
  skewCentre: 0,
  flags: 0,
  choices: [],
});

export const ZONE_SPECS = {
  volume_db: spec('volume_db', 'Vol', -60, 12, 0, 'dB'),
  pan: spec('pan', 'Pan', -1, 1, 0),
  transpose: spec('transpose', 'Trans', -48, 48, 0, 'st', 'int'),
};

const LAYER_HUES = [32, 190, 300, 95, 0, 240, 150, 60];

const LayerRow = memo(function LayerRow({ layer, index, audible, selected, count }: { layer: Layer; index: number; audible: boolean; selected: boolean; count: number }) {
  const catalog = useEngine((s) => s.catalog);
  const node = layer.node!;
  const z = layer.zone;
  const instruments = catalog.filter((m) => m.kind === 'instrument');
  const hue = LAYER_HUES[index % LAYER_HUES.length]!;
  return (
    <div
      className={`layer ${selected ? 'on' : ''} ${audible ? '' : 'silent'}`}
      style={{ ['--layer-hue' as string]: hue }}
      onClick={() => actions().selectLayer(node)}
    >
      <div className="layer-stripe" />
      <div className="layer-id">
        <div className="layer-num mono">{String(index + 1).padStart(2, '0')}</div>
        <div className="layer-meta">
          <div className="layer-name">{layer.name}</div>
          <select
            className="select tiny"
            value={layer.instrument.type}
            onClick={(e) => e.stopPropagation()}
            onChange={(e) => void actions().setInstrument(node, e.target.value)}
            aria-label="Instrument"
          >
            {instruments.map((m) => (
              <option key={m.typeId} value={m.typeId}>
                {m.name}
              </option>
            ))}
            {!instruments.some((m) => m.typeId === layer.instrument.type) && <option value={layer.instrument.type}>{layer.instrument.type}</option>}
          </select>
        </div>
      </div>
      <ZoneEditor lo={z.key_lo} hi={z.key_hi} dimmed={!audible} onChange={(lo, hi) => actions().setZone(node, { key_lo: lo, key_hi: hi })} />
      <div className="layer-knobs" onClick={(e) => e.stopPropagation()}>
        <Knob spec={ZONE_SPECS.transpose} value={z.transpose} size={30} onChange={(v) => actions().setZone(node, { transpose: v })} />
        <Knob spec={ZONE_SPECS.volume_db} value={z.volume_db} size={30} onChange={(v) => actions().setZone(node, { volume_db: v })} />
        <Knob spec={ZONE_SPECS.pan} value={z.pan} size={30} onChange={(v) => actions().setZone(node, { pan: v })} />
      </div>
      <div className="layer-ms" onClick={(e) => e.stopPropagation()}>
        <button type="button" className={`ms mute ${z.mute ? 'on' : ''}`} aria-pressed={z.mute} onClick={() => actions().setZone(node, { mute: !z.mute })} title="Mute">
          M
        </button>
        <button type="button" className={`ms solo ${z.solo ? 'on' : ''}`} aria-pressed={z.solo} onClick={() => actions().setZone(node, { solo: !z.solo })} title="Solo">
          S
        </button>
      </div>
      <StereoMeter source={layerSource(node)} orientation="horizontal" className="layer-meter" />
      <button
        type="button"
        className="icon-btn"
        disabled={count <= 1}
        aria-label="Remove layer"
        title="Remove layer"
        onClick={(e) => {
          e.stopPropagation();
          void actions().removeLayer(node);
        }}
      >
        ×
      </button>
    </div>
  );
});

function ChainStrip() {
  const layerNode = useEngine((s) => s.selectedLayer);
  const layer = useEngine((s) => (layerNode === null ? null : findLayer(s.patch, layerNode)));
  const selected = useEngine((s) => s.selectedModule);
  const instName = useEngine((s) => (layer ? (s.catalogMap[layer.instrument.type]?.name ?? layer.instrument.type) : ''));
  if (!layer) return null;
  const inst = layer.instrument;
  return (
    <div className="chain">
      <div className="panel-label">Signal chain · {layer.name}</div>
      <div className="chain-row">
        <button
          type="button"
          className={`chain-inst ${selected === inst.node ? 'on' : ''}`}
          onClick={() => inst.node !== undefined && actions().selectModule(inst.node, layer.node)}
        >
          <span className="kind-tag instrument">INST</span>
          {instName}
        </button>
        <span className="chain-arrow" aria-hidden>
          →
        </span>
        <FxChain layer={layer.node!} fx={layer.fx} {...(layer.node !== undefined ? { selectLayer: layer.node } : {})} />
      </div>
    </div>
  );
}

export function Rack() {
  const patch = useEngine((s) => s.patch);
  const selectedLayer = useEngine((s) => s.selectedLayer);
  if (!patch) {
    return (
      <main className="rack">
        <div className="panel empty-state">Waiting for engine state…</div>
      </main>
    );
  }
  return (
    <main className="rack">
      <section className="layers panel">
        <div className="layers-head">
          <span className="panel-label">Layers</span>
          <span className="spacer" />
          <button type="button" className="btn small" onClick={() => void actions().addLayer()}>
            + Layer
          </button>
        </div>
        <div className="layers-list">
          {patch.layers.map((l, i) => (
            <LayerRow
              key={l.node ?? i}
              layer={l}
              index={i}
              count={patch.layers.length}
              audible={isAudible(patch, l)}
              selected={l.node === selectedLayer}
            />
          ))}
        </div>
      </section>
      <ChainStrip />
      <ModulePanel />
    </main>
  );
}
