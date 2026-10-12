// Right-hand drawer: account / sync card + sample library.

import { useEffect, useRef } from 'react';
import { cloudActions } from '../../store';
import { AccountCard } from './AccountCard';
import { SampleLibrary } from './SampleLibrary';

export function CloudDrawer({ open, onClose }: { open: boolean; onClose: () => void }) {
  const ref = useRef<HTMLDialogElement>(null);
  useEffect(() => {
    const d = ref.current;
    if (!d) return;
    if (open && !d.open) {
      d.showModal();
      void cloudActions().refreshSamples();
    } else if (!open && d.open) d.close();
  }, [open]);

  return (
    <dialog
      ref={ref}
      className="drawer"
      onClose={onClose}
      onCancel={onClose}
      onClick={(e) => {
        if (e.target === ref.current) onClose(); // click on the backdrop
      }}
      aria-label="Облако и сэмплы"
    >
      <header className="dialog-head">
        <h2>Облако и сэмплы</h2>
        <button type="button" className="icon-btn" onClick={onClose} aria-label="Закрыть">
          ×
        </button>
      </header>
      <div className="drawer-body">
        {open && (
          <>
            <AccountCard />
            <SampleLibrary />
          </>
        )}
      </div>
    </dialog>
  );
}
