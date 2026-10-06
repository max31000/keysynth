import { memo, useMemo, useState } from 'react';
import { actions, useEngine } from '../store';
import { findModule } from '../store/patchOps';
import { FRONT_TAB, groupParams, panelTabs, type ParamGroup } from '../lib/panelLayout';
import type { ModuleInfo, ParamSpec } from '../protocol/types';
import { ParamControl } from './Controls';

// Selected tab per module type, kept for the session (switching presets keeps you on "Op 3").
const lastTab = new Map<string, string>();

const ParamCell = memo(function ParamCell({
  node,
  spec,
  style,
}: {
  node: number;
  spec: ParamSpec;
  style?: 'knob' | 'slider' | 'drawbar';
}) {
  const value = useEngine((s) => findModule(s.patch, node)?.slot.params[spec.id] ?? spec.def);
  return (
    <ParamControl
      node={node}
      spec={spec}
      value={value}
      onChange={(v) => actions().setParam(node, spec.id, v)}
      {...(style ? { style } : {})}
    />
  );
});

function Groups({ node, groups, info, bare }: { node: number; groups: ParamGroup[]; info: ModuleInfo; bare?: boolean }) {
  const controls = info.uiHints?.controls ?? {};
  return (
    <div className="groups">
      {groups.map((g) => {
        const drawbars = g.params.every((p) => controls[p.id] === 'drawbar');
        return (
          <fieldset key={g.name} className={`group ${drawbars ? 'group-drawbars' : ''} ${bare ? 'group-bare' : ''}`}>
            {!bare && <legend>{g.name}</legend>}
            <div className="group-body">
              {g.params.map((p) => (
                <ParamCell key={p.id} node={node} spec={p} {...(controls[p.id] ? { style: controls[p.id] } : {})} />
              ))}
            </div>
          </fieldset>
        );
      })}
    </div>
  );
}

/**
 * Auto-generated panel built from the module's ParamSpec list: the selected module, or `node` when given
 * (the Rhythm panel shows the drum-sequencer kit this way). Large modules (fm, va, drums) are tabbed.
 */
export function ModulePanel({ node: fixedNode }: { node?: number | null } = {}) {
  const selected = useEngine((s) => s.selectedModule);
  const node = fixedNode !== undefined ? fixedNode : selected;
  const type = useEngine((s) => (node === null ? undefined : findModule(s.patch, node)?.slot.type));
  const kind = useEngine((s) => (node === null ? undefined : findModule(s.patch, node)?.kind));
  const layerName = useEngine((s) => (node === null ? undefined : findModule(s.patch, node)?.layer?.name));
  const info = useEngine((s) => (type ? s.catalogMap[type] : undefined));
  const groups = useMemo(() => (info ? groupParams(info) : []), [info]);
  const tabs = useMemo(() => (info ? panelTabs(info, groups) : null), [info, groups]);
  const [, rerender] = useState(0);

  if (node === null || !type) return <section className="module panel empty-state">Select an instrument or effect.</section>;
  if (!info)
    return (
      <section className="module panel empty-state">
        Module <code>{type}</code> is not in the engine catalog.
      </section>
    );

  const wanted = lastTab.get(info.typeId);
  const current = tabs ? (tabs.find((t) => t.name === wanted) ?? tabs[0]!) : null;
  const pick = (name: string) => {
    lastTab.set(info.typeId, name);
    rerender((n) => n + 1);
  };

  return (
    <section className={`module panel ${tabs ? 'tabbed' : ''}`} aria-label={`${info.name} parameters`}>
      <header className="module-head">
        <span className={`kind-tag ${kind}`}>{kind === 'instrument' ? 'INST' : kind === 'rhythm' ? 'RHY' : 'FX'}</span>
        <h2 className="module-name">{info.name}</h2>
        <span className="module-type mono">{info.typeId}</span>
        <span className="module-where">
          {layerName ? `on ${layerName}` : kind === 'rhythm' ? 'drum sequencer' : 'master chain'}
        </span>
        <span className="module-node mono">#{node}</span>
      </header>
      {tabs && current ? (
        <>
          <div className="module-tabs" role="tablist" aria-label={`${info.name} sections`}>
            {tabs.map((t) => (
              <button
                type="button"
                role="tab"
                key={t.name}
                aria-selected={t === current}
                className={t === current ? 'on' : ''}
                onClick={() => pick(t.name)}
              >
                {t.name}
              </button>
            ))}
          </div>
          <div className="module-body" role="tabpanel" aria-label={current.name}>
            <Groups node={node} groups={current.groups} info={info} bare={current.name === FRONT_TAB} />
          </div>
        </>
      ) : (
        <div className="module-body">
          <Groups node={node} groups={groups} info={info} />
        </div>
      )}
    </section>
  );
}
