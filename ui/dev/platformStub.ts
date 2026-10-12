// Dev-only stand-in for the platform endpoints the keysynth page uses (`npm run dev:platform`):
//   /auth/api/me, /auth/api/refresh, /auth/api/logout, /?login=1&return=…  (mvv42-auth, kit platform client)
//   /keysynth/api/devices  POST / GET / DELETE                             (docs/specs/2026-10-12-cloud-api.md §3.1)
//   /__stub/github-latest                                                  (GitHub latest release, for the banner)
// Controls (open in the browser):  /__stub/login  /__stub/logout  /__stub/account?id=other|me
//                                  /__stub/latest?v=0.2.0 (or v= to clear)  /__stub/state
// Tokens are fake (`ksd_` + 43 chars). Mock-engine conventions: "OTHER" in a token = account 42 (mock-engine.ts).
// Never used in production builds.

import { randomBytes } from 'node:crypto';
import type { IncomingMessage, ServerResponse } from 'node:http';
import type { Plugin } from 'vite';

interface StubState {
  loggedIn: boolean;
  account: 'me' | 'other';
  latest: string | null;
  /** per account (server rows are per user + device) */
  devices: Record<'me' | 'other', Map<string, { deviceId: string; name: string; createdAt: string; lastSeenAt: string | null; expiresAt: string }>>;
}

const MOCK_EXPIRES_AT = '2027-04-10T12:00:00Z';
const B64URL ='abcdefghijklmnopqrstuvwxyz0123456789-_';
function fakeToken(other: boolean): string {
  const bytes = randomBytes(43);
  let body = '';
  for (const b of bytes) body += B64URL[b % B64URL.length];
  return `ksd_${other ? 'OTHER' + body.slice(5) : body}`;
}

function json(res: ServerResponse, status: number, body?: unknown) {
  res.statusCode = status;
  res.setHeader('Cache-Control', 'no-store');
  if (body === undefined) return res.end();
  res.setHeader('Content-Type', 'application/json');
  res.end(JSON.stringify(body));
}

function readBody(req: IncomingMessage): Promise<string> {
  return new Promise((resolve) => {
    let s = '';
    req.on('data', (c: Buffer) => (s += c.toString('utf8')));
    req.on('end', () => resolve(s));
  });
}

export function platformStub(opts: { latest?: string | null } = {}): Plugin {
  const st: StubState = { loggedIn: true, account: 'me', latest: opts.latest ?? null, devices: { me: new Map(), other: new Map() } };
  const user = () =>
    st.account === 'me'
      ? { id: 1, telegramId: 1001, name: 'Max', username: 'max', photoUrl: null, isAdmin: true, isOwner: true }
      : { id: 2, telegramId: 1002, name: 'Other', username: 'other', photoUrl: null, isAdmin: false, isOwner: false };

  return {
    name: 'keysynth-platform-stub',
    apply: 'serve',
    configureServer(server) {
      server.middlewares.use(async (req, res, next) => {
        const url = new URL(req.url ?? '/', 'http://stub');
        const p = url.pathname;
        const back = () => {
          res.statusCode = 302;
          res.setHeader('Location', '/keysynth/');
          res.end();
        };

        if (p === '/' && url.searchParams.get('login') === '1') {
          st.loggedIn = true;
          res.statusCode = 302;
          const ret = url.searchParams.get('return') ?? '/keysynth/';
          res.setHeader('Location', ret.startsWith('/') && !ret.startsWith('//') ? ret : '/keysynth/');
          return res.end();
        }
        if (p === '/__stub/login') return (st.loggedIn = true), back();
        if (p === '/__stub/logout') return (st.loggedIn = false), back();
        if (p === '/__stub/account') return (st.account = url.searchParams.get('id') === 'other' ? 'other' : 'me'), back();
        if (p === '/__stub/latest') return (st.latest = url.searchParams.get('v') || null), json(res, 200, { latest: st.latest });
        if (p === '/__stub/state') return json(res, 200, { ...st, devices: { me: [...st.devices.me.values()], other: [...st.devices.other.values()] } });
        if (p === '/__stub/github-latest') {
          if (!st.latest) return json(res, 404, { message: 'Not Found' });
          return json(res, 200, {
            tag_name: `v${st.latest}`,
            html_url: `https://github.com/max31000/keysynth/releases/tag/v${st.latest}`,
            draft: false,
            prerelease: false,
          });
        }

        if (p === '/auth/api/me') {
          return json(res, 200, {
            user: st.loggedIn ? user() : null,
            apps: [{ id: 'keysynth', anonymous: true, access: 'invite', granted: st.loggedIn }],
          });
        }
        if (p === '/auth/api/refresh') return json(res, st.loggedIn ? 200 : 401, st.loggedIn ? { ok: true } : { error: 'unauthorized' });
        if (p === '/auth/api/logout') return (st.loggedIn = false), json(res, 204);

        if (p === '/keysynth/api/devices' || p.startsWith('/keysynth/api/devices/')) {
          if (!st.loggedIn) return json(res, 401, { error: 'unauthorized' });
          if (req.method === 'POST' && p === '/keysynth/api/devices') {
            let body: { deviceId?: unknown; name?: unknown } = {};
            try {
              body = JSON.parse(await readBody(req)) as typeof body;
            } catch {
              return json(res, 400, { error: 'invalid_request' });
            }
            if (typeof body.deviceId !== 'string' || typeof body.name !== 'string' || !body.name.trim())
              return json(res, 400, { error: 'invalid_request' });
            const now = new Date();
            // = the mock engine's fixed link expiry (it cannot ask a server), so the page's owner check matches
            const expiresAt = MOCK_EXPIRES_AT;
            const mine = st.devices[st.account];
            const existed = mine.has(body.deviceId);
            mine.set(body.deviceId, {
              deviceId: body.deviceId,
              name: body.name.trim(),
              createdAt: now.toISOString(),
              lastSeenAt: null,
              expiresAt,
            });
            const u = user();
            return json(res, existed ? 200 : 201, {
              deviceId: body.deviceId,
              token: fakeToken(st.account === 'other'),
              expiresAt,
              accountId: st.account === 'me' ? '17' : '42',
              accountName: u.name,
            });
          }
          if (req.method === 'GET' && p === '/keysynth/api/devices') return json(res, 200, { items: [...st.devices[st.account].values()] });
          if (req.method === 'DELETE') {
            const id = decodeURIComponent(p.slice('/keysynth/api/devices/'.length));
            return st.devices[st.account].delete(id) ? json(res, 204) : json(res, 404, { error: 'not_found' });
          }
          return json(res, 405, { error: 'invalid_request' });
        }
        next();
      });
    },
  };
}
