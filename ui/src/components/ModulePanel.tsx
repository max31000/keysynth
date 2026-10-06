import { memo, useMemo } from 'react';
import { actions, useEngine } from '../store';
import { findModule } from '../store/patchOps';
import { isHidden } from '../lib/paramMath';
import type { ModuleInfo, ParamSpec } from '../protocol/types';
import { ParamControl } from './Controls';

/** Groups in display order: uiHints.groupOrder first, then first-appearance order. */
export function groupParams(info: ModuleInfo): { name: string; params: ParamSpec[] }[] {
  const map = new Map<string, ParamSpec[]>();
  for (const p of info.params) {
    if (isHidden(p)) continue;
    const g = p.group || 'Main';
    const list = map.get(g) ?? [];
    list.push(p);
    map.set(g, list);
  }
  const order = info.uiHints?.groupOrder ?? [];
  const names = [...order.filter((g) => map.has(g)), ...[...map.keys()].filter((g) => !order.includes(g))];
  return names.map((name) => ({ name, params: map.get(name)! }));
}

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

/** Auto-generated panel for the selected module, built from its ParamSpec list. */
export function ModulePanel() {
  const node = useEngine((s) => s.selectedModule);
  const type = useEngine((s) => (node === null ? undefined : findModule(s.patch, node)?.slot.type));
  const kind = useEngine((s) => (node === null ? undefined : findModule(s.patch, node)?.kind));
  const layerName = useEngine((s) => (node === null ? undefined : findModule(s.patch, node)?.layer?.name));
  const info = useEngine((s) => (type ? s.catalogMap[type] : undefined));
  const groups = useMemo(() => (info ? groupParams(info) : []), [info]);

  if (node === null || !type) return <section className="module panel empty-state">Select an instrument or effect.</section>;
  if (!info)
    return (
      <section className="module panel empty-state">
        Module <code>{type}</code> is not in the engine catalog.
      </section>
    );

  const controls = info.uiHints?.controls ?? {};
  return (
    <section className="module panel" aria-label={`${info.name} parameters`}>
      <header className="module-head">
        <span className={`kind-tag ${kind}`}>{kind === 'instrument' ? 'INST' : 'FX'}</span>
        <h2 className="module-name">{info.name}</h2>
        <span className="module-type mono">{info.typeId}</span>
        <span className="module-where">{layerName ? `on ${layerName}` : 'master chain'}</span>
        <span className="module-node mono">#{node}</span>
      </header>
      <div className="groups">
        {groups.map((g) => {
          const drawbars = g.params.every((p) => controls[p.id] === 'drawbar');
          return (
            <fieldset key={g.name} className={`group ${drawbars ? 'group-drawbars' : ''}`}>
              <legend>{g.name}</legend>
              <div className="group-body">
                {g.params.map((p) => (
                  <ParamCell key={p.id} node={node} spec={p} {...(controls[p.id] ? { style: controls[p.id] } : {})} />
                ))}
              </div>
            </fieldset>
          );
        })}
      </div>
    </section>
  );
}
