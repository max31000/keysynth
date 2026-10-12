# mvv42.ru platform — how services are built and added (research, 2026-10-12)

Read-only survey of `M:\Projects\mvv42-infra`, `mvv42-auth`, `mvv42-kit`, `portalShell`, `bonds`, `cashpulse`,
`it_credit_calc`, `SoundLeveling`. Goal: add **keysynth** (web UI + Windows engine download + sample library +
preset sync) the same way as existing services. Source of truth is always the platform repos; paths below are
absolute so they can be re-checked. `secrets.env` was **not** read (only `secrets.env.example`).

Repos (GitHub user `max31000`):

| Local dir | Remote | Role | Recent direction |
|---|---|---|---|
| `M:\Projects\mvv42-infra` | `max31000/mvv42-infra` (**private**) | nginx config, reusable deploy workflows, scaffolder, platform docs | closed-registration spec, cashpulse legacy route cleanup |
| `M:\Projects\mvv42-auth` | `max31000/mvv42-auth` (public) | shared auth service (.NET 8) + SDKs | closed registration (branch), admin API |
| `M:\Projects\mvv42-kit` | `max31000/mvv42-kit` | `@mvv42/kit` design kit + platform client (npm tarball on GitHub Releases), v0.1.3 | components |
| `M:\Projects\portalShell` | `max31000/portal-shell` | shell SPA at `/` | invite-only login (branch), admin console |
| `M:\Projects\bonds` | `max31000/bonds` | service: .NET API + React SPA, single `deploy.yml` | shared auth + kit switch done |
| `M:\Projects\cashpulse` | `max31000/cashpulse` | service: .NET API + React SPA | shared auth + kit done |
| `M:\Projects\it_credit_calc` | `max31000/it_credit_calc` | service, compose-based exception | shared auth + kit done |

Not platform services: `M:\Projects\financeCounter` is an old clone of the cashpulse repo (same remote);
`M:\Projects\max-tg-bridge` is not a git repo (research notes only); `M:\Projects\SoundLeveling` has deploy docs
for `/sound/` on port 5002 but is **not deployed** (PROPOSALS.md #25).

`M:\Projects\mvv42-infra\repos.txt` currently lists: mvv42-infra, portal-shell, mvv42-auth, cashpulse, bonds,
it_credit_calc.

---

## 1. Topology

- **One VDS** (Ubuntu, distro nginx 1.24 under systemd, Docker, certbot). Deploy user `root`. No k8s, no compose
  for most apps: containers are started with plain `docker run` from CI over SSH (compose only in
  `it_credit_calc/deploy/docker-compose.prod.yml`). `watchtower --cleanup --interval 3600` runs on the host and
  re-pulls every container's tag hourly unless the container has the label
  `com.centurylinklabs.watchtower.enable=false` (new apps: deploy `sha-<commit>` tags and/or set that label).
  Ref: `mvv42-infra\docs\server-layout.md`.
- **The same VDS is also the EU node of the owner's VPN** (`eu.mvv42.ru`, xray on `127.0.0.1:10001` behind a
  secret nginx path in `/etc/nginx/mvv42-local/`, never in git). Bandwidth/CPU are shared with the VPN —
  relevant for serving GBs of samples.
- **Single origin `https://mvv42.ru`, path-based routing, no subdomains for apps.** `www` → 301 apex (host-only
  auth cookies). `eu.mvv42.ru` shares the apex server block. Each app is `https://mvv42.ru/<id>/`.
- **nginx layout** (repo → server):
  - `nginx/mvv42.conf` → `/etc/nginx/sites-available/mvv42`: :80 (ACME webroot `/var/www/html` + redirect),
    :443 www→apex, :443 apex server (TLS, security headers, gzip, `root /var/www`, shell locations, then
    `include /etc/nginx/mvv42-local/*.conf;`, then `include /etc/nginx/mvv42/apps/*.conf;`, final `location / { return 404; }`).
  - `nginx/apps/<id>.conf` → `/etc/nginx/mvv42/apps/` — **one file per app**, only `^~` and `=` locations
    (no regex). Existing: `auth.conf`, `bonds.conf`, `cashpulse.conf`, `credit_calc.conf`.
  - `nginx/snippets/` → `/etc/nginx/mvv42/snippets/`: `tls.conf`, `security-headers.conf`, `spa-static.conf`
    (`Cache-Control: no-cache`), `static-immutable.conf` (1 year immutable), `proxy.conf`.
  - `nginx/conf.d/mvv42-ratelimit.conf` → `/etc/nginx/conf.d/` (http-level `limit_req_zone mvv42_auth` 10 r/m).
  - Rule: never a bare `add_header` in an app file (it drops all server-level headers) — use the snippets or
    re-include `security-headers.conf`.
- **TLS:** Let's Encrypt, `certbot --webroot -w /var/www/html`, cert `/etc/letsencrypt/live/mvv42.ru/` with SANs
  `mvv42.ru, www.mvv42.ru, eu.mvv42.ru`; settings in `nginx/snippets/tls.conf` (TLS 1.2/1.3). Never run
  `certbot --nginx`. HSTS `max-age=86400`.
- **Security headers on every response:** HSTS, `nosniff`, `Referrer-Policy: strict-origin-when-cross-origin`,
  `Content-Security-Policy: frame-ancestors 'self'` (no `script-src`/`connect-src` yet → a page may connect to
  `ws://127.0.0.1:<port>` without CSP changes).
- **nginx changes** go PR → `nginx-check.yml` (nginx -t on nginx:1.24 + header smoke) → merge → manual workflow
  *apply nginx* (`apply-nginx.yml` → `scripts/apply-nginx-remote.sh`: backup, install, `nginx -t`, upstream
  safety net, reload, smoke, auto-rollback, Telegram alert).

## 2. Registering a new service end-to-end

Docs: `mvv42-infra\docs\new-app.md`, `mvv42-infra\docs\conventions.md`.

| Item | Convention |
|---|---|
| App id | `^[a-z][a-z0-9_-]{0,63}$`; same id in nginx, `/var/www/<id>`, `portal-shell/public/registry.json`, `mvv42_auth.apps`. Reserved: `auth, api, assets, health, .well-known, portal` (+ `admin` in auth). |
| URLs | `/<id>/` SPA (fallback `/<id>/index.html`, `no-cache`), `/<id>/assets/` immutable, `/<id>/api/` → `127.0.0.1:<port>/api/` (**prefix stripped** — app routes start at `/api/`), `/<id>/health` → `:<port>/health`. |
| Ports | API binds `127.0.0.1:<port>` only. Taken: 5000 cashpulse-api, 5001 bonds-api, 5010 mvv42-auth, 8081 credit_calc; 5002 is claimed by the undeployed SoundLeveling docs (not in `nginx/apps`, so `new-app.sh` would not notice). Use e.g. **5020**. |
| Container | name `<id>-api`, image `ghcr.io/max31000/<name>`, run tag `sha-<commit>`; network `cashpulse-network` (needed for MySQL `cashpulse-mysql` and JWKS `http://mvv42-auth:8080`). |
| Env | `/etc/mvv42/env/<container>.env` (dir 700, file 600) written by the deploy; container started with `--env-file`. |
| Data | bind mounts under `/var/lib/<id>/...` |
| Health | `GET /health` → 2xx when ready; deploy polls `http://127.0.0.1:<port><health-path>`. |
| Logs | only `docker logs <container>` (deploy prints last 60–80 lines on failure). No central logging/monitoring (PROPOSALS #14). Failures → Telegram via `TELEGRAM_BOT_TOKEN`/`TELEGRAM_CHAT_ID`. |

Steps:
1. In mvv42-infra: `scripts/new-app.sh keysynth 5020` → writes `nginx/apps/keysynth.conf` and prints the
   registry entry and `INSERT INTO apps (id, name, anonymous, access) VALUES ('keysynth','keysynth',0,'invite');`.
   PR → nginx check → merge → *apply nginx*.
2. App repo: Vite `base: '/keysynth/'`, API calls to `/keysynth/api/...`; add the workflows (§3).
3. Add the repo to `mvv42-infra\repos.txt` (`max31000/keysynth VDS_HOST,VDS_USER,VDS_SSH_KEY,TELEGRAM_BOT_TOKEN,TELEGRAM_CHAT_ID`)
   and run `scripts/sync-secrets.sh` (owner, locally).
4. DB (if any): manual `CREATE DATABASE/USER` in `cashpulse-mysql` (`docs/server-layout.md`), connection
   string → repo secret `DB_CONNECTION_STRING`.
5. Shell: add the entry to `portalShell/public/registry.json` (with `"auth": "shared"`); icon must be in the
   allow-list `portalShell/src/components/icons.ts` (no music/piano icon there yet — add e.g. `IconPiano`).
6. Auth: insert the `apps` row (or `PUT /auth/api/admin/apps/keysynth` / shell admin «Сервисы») and integrate
   the SDK (§4).

## 3. CI/CD

- **GitHub Actions only**, push to `main` → test → build → deploy. Registry: **GHCR** (`ghcr.io/max31000/...`),
  pushed with the job's `GITHUB_TOKEN` (`packages: write`). Deploy: **SSH with a deploy key** via
  `appleboy/ssh-action@v1` / `appleboy/scp-action@v1` (secrets `VDS_HOST`, `VDS_USER`, `VDS_SSH_KEY`). No webhooks.
  For private GHCR packages the deploy does `docker login ghcr.io` on the server with the job token, then logs out.
- **Reusable workflows** in `mvv42-infra/.github/workflows/` (callable because of the user-level Actions access
  setting on the private repo): `deploy-static.yml` (SPA → `/var/www/<id>`, atomic dir swap, previous release
  kept as `/var/www/.<id>.prev`, old hashed assets survive one release) and `deploy-dotnet-api.yml` (.NET tests →
  image `latest`+`sha-` → container on `127.0.0.1:<port>`, health poll, rollback to `<name>-prev`). Secrets are
  passed **explicitly**, never `secrets: inherit`; app secrets go in one multi-line secret `APP_ENV`
  (single-line `KEY=VALUE`s).
- Existing services (bonds, cashpulse, credit_calc) still use their own single `deploy.yml`
  (`changes` → `backend` → `frontend`, `dorny/paths-filter`, sha tags, rollback, JWKS reachability check);
  PROPOSALS #32 wants that turned into a template repo. New apps per docs: use the reusable workflows.

Representative callers (verbatim from `mvv42-infra\docs\new-app.md`):

```yaml
# .github/workflows/frontend.yml in the app repo
name: Frontend
on:
  push:
    branches: [main]
    paths: ['web/**', '.github/workflows/frontend.yml']
  workflow_dispatch:

jobs:
  deploy:
    uses: max31000/mvv42-infra/.github/workflows/deploy-static.yml@main
    with:
      app-id: myapp
      working-directory: web
      package-manager: yarn          # npm | yarn | pnpm
      test-command: yarn typecheck && yarn test:run
      build-env: |
        VITE_API_BASE=/myapp/api
    secrets:
      VDS_HOST: ${{ secrets.VDS_HOST }}
      VDS_USER: ${{ secrets.VDS_USER }}
      VDS_SSH_KEY: ${{ secrets.VDS_SSH_KEY }}
      TELEGRAM_BOT_TOKEN: ${{ secrets.TELEGRAM_BOT_TOKEN }}
      TELEGRAM_CHAT_ID: ${{ secrets.TELEGRAM_CHAT_ID }}
```

```yaml
# .github/workflows/backend.yml in the app repo
name: Backend
on:
  push:
    branches: [main]
    paths: ['src/**', 'tests/**', '.github/workflows/backend.yml']
  workflow_dispatch:

jobs:
  deploy:
    uses: max31000/mvv42-infra/.github/workflows/deploy-dotnet-api.yml@main
    permissions:
      contents: read
      packages: write                # push to GHCR
    with:
      app-id: myapp
      image-name: myapp-api          # ghcr.io/<owner>/myapp-api
      container-name: myapp-api
      dockerfile: src/MyApp.Api/Dockerfile
      port: 5020                     # 127.0.0.1:5020 on the host
      container-port: 8080
      network: cashpulse-network     # default; needed for MySQL and http://mvv42-auth:8080
      test-command: dotnet test -c Release
      health-path: /health           # inside the container's URL space
      env: |
        ASPNETCORE_ENVIRONMENT=Production
        Telegram__BotUsername=mv_cashpulse_bot
      volumes: |
        /var/lib/myapp/dataprotection-keys:/app/dataprotection-keys
    secrets:
      VDS_HOST: ${{ secrets.VDS_HOST }}
      VDS_USER: ${{ secrets.VDS_USER }}
      VDS_SSH_KEY: ${{ secrets.VDS_SSH_KEY }}
      TELEGRAM_BOT_TOKEN: ${{ secrets.TELEGRAM_BOT_TOKEN }}
      TELEGRAM_CHAT_ID: ${{ secrets.TELEGRAM_CHAT_ID }}
      APP_ENV: |
        ConnectionStrings__DefaultConnection=${{ secrets.DB_CONNECTION_STRING }}
        Jwt__Secret=${{ secrets.JWT_SECRET }}
        Telegram__BotToken=${{ secrets.TELEGRAM_BOT_TOKEN }}
```

(For a shared-auth app, drop `Jwt__Secret` — no own JWT anymore.) Other `deploy-dotnet-api` inputs: `context`,
`volume-owner` (e.g. `1654:1654` for the non-root `app` user of `mcr.microsoft.com/dotnet/aspnet:8.0`),
`health-timeout` (60), `dotnet-version` (`8.0.x`). `deploy-static` inputs: `app-id`, `working-directory`,
`package-manager`, `node-version` (20), `install-command`, `test-command`, `build-command`, `dist-dir`,
`build-env`, `verify-url`.

Hand-written equivalent, the deploy step of `M:\Projects\bonds\.github\workflows\deploy.yml` (excerpt, verbatim
except line continuation layout):

```yaml
      - name: Deploy on VDS
        if: github.event_name != 'pull_request'
        uses: appleboy/ssh-action@v1
        env:
          DB_CONNECTION_STRING: ${{ secrets.DB_CONNECTION_STRING }}
          TELEGRAM_BOT_TOKEN: ${{ secrets.TELEGRAM_BOT_TOKEN }}
          OWNER_TELEGRAM_ID: ${{ secrets.OWNER_TELEGRAM_ID }}
          IMAGE: ghcr.io/${{ github.repository_owner }}/bonds-api:sha-${{ github.sha }}
        with:
          host: ${{ secrets.VDS_HOST }}
          username: ${{ secrets.VDS_USER }}
          key: ${{ secrets.VDS_SSH_KEY }}
          envs: DB_CONNECTION_STRING,TELEGRAM_BOT_TOKEN,OWNER_TELEGRAM_ID,IMAGE
          script: |
            set -e
            mkdir -p /var/lib/bonds/dataprotection-keys
            docker pull "$IMAGE"
            PREV=$(docker inspect --format '{{.Config.Image}}' bonds-api 2>/dev/null || true)
            run_api() {
              docker run -d --name bonds-api --network cashpulse-network --restart unless-stopped \
                --label com.centurylinklabs.watchtower.enable=false \
                -p 127.0.0.1:5001:5001 \
                -v /var/lib/bonds/dataprotection-keys:/app/dataprotection-keys \
                -e "ConnectionStrings__DefaultConnection=${DB_CONNECTION_STRING}" \
                -e "Telegram__BotToken=${TELEGRAM_BOT_TOKEN}" \
                -e "Telegram__OwnerId=${OWNER_TELEGRAM_ID}" \
                "$1"
            }
            healthy() { for i in $(seq 1 15); do curl -sf http://localhost:5001/health > /dev/null && return 0; sleep 2; done; return 1; }
            jwks_reachable() {
              docker run --rm --network cashpulse-network curlimages/curl:8.10.1 \
                -sf http://mvv42-auth:8080/auth/.well-known/jwks.json > /dev/null
            }
            docker stop bonds-api && docker rm bonds-api || true
            run_api "$IMAGE"
            if ! healthy || ! jwks_reachable; then
              docker logs --tail 80 bonds-api || true
              docker stop bonds-api || true; docker rm bonds-api || true
              [ -n "$PREV" ] && run_api "$PREV"
              exit 1
            fi
```

Its frontend job: `yarn install --frozen-lockfile` → `typecheck` → `lint` → `test:run` → `build`
(`VITE_API_BASE=${{ vars.VITE_API_BASE || '/bonds/api' }}`) → `appleboy/scp-action` `bonds-web/dist/*` →
`/var/www/bonds/` (`rm: true`) → `curl -sf https://mvv42.ru/bonds/`. Failure → `curl` to Telegram `sendMessage`.

### Secrets flow

- `M:\Projects\mvv42-infra\scripts\sync-secrets.sh` (bash ≥ 4, `gh auth login` with `repo` scope). Reads
  `secrets.env` (gitignored; template `secrets.env.example`): `KEY=value` or `KEY=@file:<path>` (multi-line,
  `~/` expanded; used for the SSH key). Reads `repos.txt`: `owner/repo` (all keys) or `owner/repo KEY1,KEY2`
  (only those). For each repo/key: `gh secret set KEY -R owner/repo` with the value on **stdin** (never argv).
  Flags: `--dry-run` (repo → key names), `--only KEY[,KEY]` (rotation), `-e`, `-r`. Names starting `GITHUB_`
  rejected. **Add a repo** = append a line to `repos.txt`, run the script.
- Key names in `secrets.env.example`: `VDS_HOST`, `VDS_USER`, `VDS_SSH_KEY`, `VDS_SSH_PRIVATE_KEY` (old name,
  it_credit_calc only), `TELEGRAM_BOT_TOKEN`, `TELEGRAM_CHAT_ID`, `AUTH_DB_CONNECTION_STRING`,
  `AUTH_TELEGRAM_BOT_TOKEN`, `AUTH_ADMIN_TELEGRAM_IDS`.
- A service needs (shared, synced): `VDS_HOST, VDS_USER, VDS_SSH_KEY, TELEGRAM_BOT_TOKEN, TELEGRAM_CHAT_ID`.
  Per-app, set by hand with `gh secret set` (not synced): `DB_CONNECTION_STRING`, optionally `OWNER_TELEGRAM_ID`,
  others. Non-secret build values as repo **variables** (`gh variable set`), e.g. `VITE_API_BASE`.
  Inventory: `mvv42-infra\docs\secrets.md`.

## 4. Auth (`mvv42-auth`)

Docs: `M:\Projects\mvv42-auth\README.md`, `docs\api.md`, `sdk\dotnet\README.md`; spec
`mvv42-infra\docs\specs\2026-10-11-platform-design.md` (§4, §9 overrides).

- **Login:** Telegram Login Widget (bot `mv_cashpulse_bot`, domain mvv42.ru) in the **shell** only →
  `POST /auth/api/telegram` → session in **HttpOnly cookies**. Not OIDC (no discovery), not localStorage.
  - Access: **ES256 JWT, 10 min**, cookie `__Host-mvv42_at` (HttpOnly, Secure, SameSite=Strict, Path=/).
    Claims `iss=https://mvv42.ru/auth`, `aud=mvv42` (same for all apps), `sub` (auth user id), `tg` (telegram id),
    `name`, `adm` ("true"/"false"), `apps` (array of app ids the user may use).
  - Refresh: opaque, cookie `__Secure-mvv42_rt` (Path=/auth/api, Strict), rotated each use, sliding 30 d /
    absolute 90 d, reuse detection.
  - JWKS: `/auth/.well-known/jwks.json` (internal `http://mvv42-auth:8080/auth/.well-known/jwks.json`).
  - CSRF: SameSite=Strict + `Origin`/`Referer` must be in allowed origins for unsafe methods.
- **Backend validation:** copy-in file `mvv42-auth\sdk\dotnet\Mvv42Auth.cs` (no NuGet yet):
  `builder.Services.AddMvv42Auth("<id>", o => cfg.GetSection("Mvv42Auth").Bind(o))` → JwtBearer scheme `mvv42`
  reading the token **only from the cookie**, JWKS cached (10 min, refetch on unknown kid), policy `mvv42:app`
  (`adm == "true"` or `<id> ∈ apps`), Origin filter for unsafe methods, `ClaimsPrincipal.GetMvv42User()` →
  `(AuthUserId, TelegramId, Name, IsAdmin)`. Apps provision their **local user by `tg`** (UNIQUE `telegram_id`);
  see `bonds\src\Bonds.Api\Auth\BondsAuth.cs` (`ICurrentUser`, fallback policy = app policy, `/health`
  `AllowAnonymous`). Requires the container on `cashpulse-network`. Changes of grants/blocks reach apps within
  ≤ 10 min (token TTL). Non-.NET backends would have to re-implement this (ES256 JWKS, cookie name, claims).
- **Frontend:** `@mvv42/kit/platform` — `useMe()` / `getMe()` → `GET /auth/api/me` (always 200, implicit refresh;
  `{ user: {id, telegramId, name, username, photoUrl, isAdmin, isOwner} | null, apps: [{id, anonymous, access, granted}] }`),
  `apiJson`/`mvv42Fetch` (same-origin cookies, 401 → single refresh under Web Lock `mvv42-refresh` → retry),
  `requestLogin(id)` (in shell: `AUTH_REQUIRED` postMessage; standalone: `/?login=1&return=...`),
  `<LoginRequired>`. (`sdk/web/mvv42.ts` is deprecated in favour of the kit.)
- **Access model:** table `apps(id, name, anonymous, access)`, `access ∈ public | invite | admin`; grants
  `app_grants(user_id, app_id)` given by invite codes (`/?invite=<code>` → `POST /auth/api/invites/redeem`) or
  admin; admin = `users.is_admin` OR telegram id in `Auth__AdminTelegramIds`; **owner** (`isOwner`, ids in
  `Auth__AdminTelegramIds`) is the only one who sees the admin console (`/#/admin`) and `/auth/api/admin/*`
  (users, block, sessions, apps, grants, invites, login journal).
- **Invite-only registration (personal-data law): implemented, NOT yet merged/deployed.**
  - Spec: `mvv42-infra\docs\specs\2026-10-12-closed-registration.md` (on infra `main`, incl. review decisions):
    `Auth__Registration=invite` (default) — an unknown Telegram user is rejected (`403 invite_required`) unless
    `X-Invite-Code` carries a valid code; nothing is stored about rejected strangers (`login_events` row with
    NULL PII, `denied_unregistered`); login-only invites (`app_id NULL`); codes `XXXX-XXXX-XXXX` Crockford
    base32, HMAC-SHA256 with a server pepper; migrations 007/008.
  - `mvv42-auth`: branch `feat/closed-registration` (8 commits ahead of `main`, 6 pushed to origin, plus
    uncommitted edits in `InviteCodeHasher.cs`, `Program.cs`, tests). `main` = admin API merged (PR #1).
  - `portalShell`: branch `feat/invite-login` (3 commits ahead of `main`: invite field in login modal,
    login-only invites in admin, review fixes). Rollout order: auth first, then shell.
  - nginx already rate-limits `/auth/api/invites/redeem`.
  - Implication for keysynth: register the app with `access='invite'` (or `admin` while private); no own user
    registration; store data keyed by `tg`/local user id only after a successful shared-auth login.

## 5. Shell and kit

- **portalShell** (`M:\Projects\portalShell\README.md`): React 19 + Vite + Mantine 9 + `@mvv42/kit` SPA at `/`
  (`/var/www/portal`). Not module federation: each service is a **standalone SPA loaded in a same-origin
  `<iframe src="/<id>/">`**, hash routing `#/<id><path>`. Owns top bar, launcher, home, sign-in, theme, admin.
  - Listing: `public/registry.json` (UI catalogue only): `{ id, name, description, icon (Tabler name from
    src/components/icons.ts allow-list), path: "/<id>/", color, auth: "shared" }`. Lock on a tile =
    `!anonymous && !granted` from `/me`.
  - postMessage protocol (same origin only, `src/shell/messages.ts`): service→shell `READY`, `NAVIGATE`,
    `AUTH_REQUIRED`; shell→service `THEME`, `AUTH_CHANGED`. The kit's `Mvv42Provider`/`AppLayout` do this.
  - iframe (`src/components/ServiceFrame.tsx` line ~119):
    `sandbox="allow-scripts allow-same-origin allow-forms allow-popups allow-downloads allow-modals allow-popups-to-escape-sandbox"`,
    **no `allow=` attribute** → inside the shell, Web MIDI (`allow="midi"`), and any other permission-policy
    feature are blocked by default; keysynth needs a shell change (e.g. per-service `allow` in registry) or must
    be opened standalone (`https://mvv42.ru/keysynth/`). Downloads are allowed.
  - Shell deploy: own `.github/workflows/deploy.yml` (npm ci → lint → test → build → scp to `/var/www/portal/`).
- **mvv42-kit** (`M:\Projects\mvv42-kit\README.md`), `@mvv42/kit` v0.1.3, installed from a GitHub Release
  tarball URL (`https://github.com/max31000/mvv42-kit/releases/download/v0.1.3/mvv42-kit-0.1.3.tgz`).
  Frontend only (no backend libs; backend auth is `Mvv42Auth.cs`). Entries: `@mvv42/kit` (`Mvv42Provider`,
  `Page`, `Section`, `Metric(Grid)`, `DataList/ListRow`, `EmptyState`, `ErrorState`, `LoginRequired`,
  `AccountButton`, `ColorSchemeToggle`, tokens), `/router` (`AppLayout`, `useShellRouterSync`), `/platform`
  (auth client + shell bridge, React-only), `/charts` (recharts 3 helpers), `/vite` (`mvv42Html()` anti-flash
  colour-scheme script), `/testing`, `/styles.css` (Inter font, `--mvv-*` tokens light/dark, Mantine overrides).
  Peers: React 19, Mantine ≥ 9.4, optional react-router-dom 7, recharts 3. Design language: zinc neutrals,
  hairlines, tabular figures, one accent per service. Recipe: README «Building a new service».

## 6. Storage

- **Database:** one shared **MySQL** container `cashpulse-mysql` on `cashpulse-network` holding per-app
  databases (`cashpulse`, `bonds`, `mvv42_auth`); one DB + user per app, created manually; apps run their own
  SQL migrations on startup (Dapper/MySqlConnector). credit_calc has its own `mysql:8.0` in compose. **No
  Postgres.** No automated backups visible (PROPOSALS #13).
- **Files:** no S3/MinIO, no object storage, no CDN. Persistent data = host bind mounts `/var/lib/<id>/`.
  Static SPAs in `/var/www/<id>`. Disk size/free space is not documented anywhere and not captured by
  `export-live.yml` — must be checked on the server (owner) before planning 4–10 GB.
- **Body size:** nginx default `client_max_body_size` 1 MB; cashpulse raises it to `6m` per location.
- **Large static files:** nothing does it today. nginx (distro build) serves static files with byte-range
  support by default; gzip only applies to listed text types. There is no `auth_request` / `X-Accel-Redirect`
  usage yet.

## 7. Backend/frontend conventions

- Backend: **.NET 8 ASP.NET Core minimal APIs**, `mcr.microsoft.com/dotnet/aspnet:8.0` image, layered
  `src/<App>.Api|Core|Infrastructure`, xUnit unit + integration tests (`WebApplicationFactory`), camelCase JSON
  + string enums, errors `{ "error": "<code>" }`, `/health` anonymous. Listens on container port (5001 bonds,
  8080 for reusable-workflow apps).
- API prefix: public `/<id>/api/...`; nginx rewrites to `/api/...` (auth is the exception: keeps `/auth`).
- CORS: unnecessary in prod (same origin); bonds keeps a config-driven CORS policy defaulting to
  `http://localhost:5173` for dev. Origin check for unsafe methods comes from the SDK.
- Frontend: React 19 + Vite + TypeScript + Mantine 9 + `@mvv42/kit` + zustand; yarn (bonds) or npm; vitest,
  MSW; Vite `base: '/<id>/'`, `BrowserRouter basename="/<id>"`.
- **WebSockets: none on the platform.** `nginx/snippets/proxy.conf` sets `proxy_http_version 1.1` and
  `Connection ""` (no `Upgrade`), so a WS location would need its own header set (not the snippet; the snippet's
  `proxy_set_header`s replace inherited ones and a second `Connection` would conflict). The keysynth UI talking to
  the **local** engine (`ws://127.0.0.1`) does not touch the server; CSP allows it (no `connect-src`). Browser
  rules for public-site → localhost (Chrome Private/Local Network Access prompts, especially from an iframe)
  need testing.

## 8. Notes for keysynth

- **Windows engine binary:** owner prefers GitHub Releases; platform precedent exists (the kit itself is
  distributed as a Release asset). Link from the UI to
  `https://github.com/max31000/<repo>/releases/latest/download/<asset>`; no server storage, no nginx change.
  If the repo is private, release assets need auth → the repo (or a separate public "releases" repo) must be
  public for direct links.
- **Sample library 4–10 GB behind auth:** no existing pattern. Options, in platform style:
  1. Files on the VDS under `/var/lib/keysynth/samples` (or `/var/www/...` outside public roots), nginx
     `location ^~ /keysynth/samples/ { internal; alias ...; }` + the API authorises and answers with
     `X-Accel-Redirect` (nginx then serves with range requests/resume, no proxy buffering). Browser requests
     carry the `__Host-mvv42_at` cookie (Path=/), so web-initiated downloads work.
  2. The **desktop engine cannot use the HttpOnly SameSite cookies** → needs its own credential: e.g. the API
     issues short-lived signed download URLs (HMAC token in the path/query, validated by the API before
     `X-Accel-Redirect`, or nginx `secure_link`), or a device token paired from the logged-in web UI.
  3. Concerns: disk size unknown; bandwidth shared with the VPN node; no CDN; consider GitHub Releases
     (2 GB per asset limit) or external object storage if the server is too small. Exclude these paths from
     gzip (default types already exclude binaries).
- **Per-user presets (small JSON) and Faust plugin sources (text):** fit the existing pattern — a `keysynth`
  DB in `cashpulse-mysql`, tables keyed by local user id (provisioned from `tg`), JSON/TEXT columns, API at
  `/keysynth/api/presets`, `/keysynth/api/plugins`; keep payloads under nginx's 1 MB default or set
  `client_max_body_size` in `nginx/apps/keysynth.conf`. Alternative: files under `/var/lib/keysynth/users/<id>/`.
- **Access:** `apps` row with `anonymous=1` would let the UI run signed-out (local engine use) while sync and
  sample downloads require login + grant; `access='invite'` matches the closed-registration model.

---

## How to add a new service — checklist (keysynth)

1. Choose id `keysynth` (valid, not reserved) and port (e.g. `5020`; 5000/5001/5010/8081 taken, 5002 claimed by SoundLeveling docs).
2. Create repo `max31000/keysynth` (or per owner preference) with: `web/` (Vite React TS, `base: '/keysynth/'`,
   `@mvv42/kit` release tarball, `mvv42Html()`, `Mvv42Provider serviceId="keysynth"`, `AppLayout`, `apiJson` to
   `/keysynth/api`), `src/Keysynth.Api` (.NET 8, `Mvv42Auth.cs` copied from `mvv42-auth/sdk/dotnet`, routes
   under `/api`, anonymous `/health`, Dockerfile), tests.
3. Workflows: `.github/workflows/frontend.yml` (calls `deploy-static.yml`) and `backend.yml` (calls
   `deploy-dotnet-api.yml`, `network: cashpulse-network`, `port: 5020`, `volumes: /var/lib/keysynth/...`,
   `APP_ENV` with `ConnectionStrings__DefaultConnection`), secrets passed explicitly. Separately, a Windows
   build + GitHub Release workflow for the engine (`windows-latest`, attach the installer/zip to the release).
4. mvv42-infra: `scripts/new-app.sh keysynth 5020`; extend `nginx/apps/keysynth.conf` if needed (body size,
   internal samples location, WS); PR → *nginx check* → merge → *apply nginx*.
5. mvv42-infra `repos.txt`: `max31000/keysynth VDS_HOST,VDS_USER,VDS_SSH_KEY,TELEGRAM_BOT_TOKEN,TELEGRAM_CHAT_ID`;
   owner runs `scripts/sync-secrets.sh --dry-run` then without it.
6. Owner, on the server: create DB/user `keysynth` in `cashpulse-mysql`; `gh secret set DB_CONNECTION_STRING -R max31000/keysynth`
   (`Server=cashpulse-mysql;Port=3306;Database=keysynth;User=keysynth;Password=...;`); create
   `/var/lib/keysynth/` (samples dir, owner uid 1654 if the container runs as `app`); check free disk.
7. Auth: `INSERT INTO apps (id, name, anonymous, access) VALUES ('keysynth','KeySynth',<0|1>,'invite');`
   (or via admin console «Сервисы»); create invite codes in `/#/admin` → «Инвайты».
8. Shell: PR to `portal-shell` adding the `registry.json` entry (`"auth": "shared"`, icon added to
   `src/components/icons.ts`); if Web MIDI/audio permissions are needed inside the iframe, add `allow="midi; autoplay"`
   support to `ServiceFrame.tsx` (e.g. a registry field).
9. GHCR: make `ghcr.io/max31000/keysynth-api` public or rely on the workflow's server-side login.
10. Verify: `curl -sf https://mvv42.ru/keysynth/health`, `https://mvv42.ru/keysynth/`, open via `/#/keysynth`,
    login/no-access/invite flows, light/dark × 1440/390.
11. Record the port/container in `mvv42-infra/docs/server-layout.md` and `conventions.md` (allocated ports list).
