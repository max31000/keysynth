import { actions, useEngine } from '../store';
import { MASTER_NODE, type ParamSpec } from '../protocol/types';
import { Knob } from './Knob';
import { StereoMeter, masterSource } from './Meter';
import { FxChain } from './FxChain';

const MASTER_VOL: ParamSpec = {
  id: 'volume_db',
  name: 'Master',
  group: 'Master',
  unit: 'dB',
  min: -60,
  max: 6,
  def: -3,
  scale: 'linear',
  skewCentre: 0,
  flags: 0,
  choices: [],
};

export function MasterSection() {
  const vol = useEngine((s) => s.patch?.master.volume_db ?? MASTER_VOL.def);
  const fx = useEngine((s) => s.patch?.master.fx);
  return (
    <aside className="master panel" aria-label="Master">
      <div className="panel-label">Master</div>
      <div className="master-meter">
        <StereoMeter source={masterSource} scale />
      </div>
      <Knob spec={MASTER_VOL} value={vol} size={48} onChange={(v) => actions().setParam(MASTER_NODE, 'volume_db', v)} />
      <div className="panel-label">Master FX</div>
      {fx && <FxChain layer={MASTER_NODE} fx={fx} vertical />}
      <div className="limiter-note">Safety limiter · fixed</div>
    </aside>
  );
}
