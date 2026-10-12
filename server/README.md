# keysynth-api

.NET 8 minimal API behind `https://mvv42.ru/keysynth/api` (nginx strips `/keysynth`). Contract:
`docs/specs/2026-10-12-cloud-api.md`; design: `docs/specs/2026-10-12-cloud.md`; platform: `docs/research/MVV42_PLATFORM.md`.

Layout: `src/Keysynth.Core` (models, validation, tokens, index parsing), `src/Keysynth.Infrastructure` (Dapper/MySQL
repositories, migrations `Migrations/NNN_*.sql` applied on startup, sample index loader), `src/Keysynth.Api`
(endpoints, auth: `Auth/Mvv42Auth.cs` copied unchanged from mvv42-auth `sdk/dotnet`, `device` bearer scheme).

## Test
```
cd server
dotnet test Keysynth.sln -c Release
```
HTTP tests use `WebApplicationFactory` with in-memory repositories; `RepositoryContractTests` pins them to the MySQL
implementation, which runs against MySQL 8.0 via Testcontainers when Docker is reachable (skipped otherwise;
`KS_TESTS_DOCKER=1|0` forces run / skip; CI always runs them).

## Run locally
Needs MySQL (database `keysynth`) and, for cookie auth, a local mvv42-auth on :5010 (see appsettings.Development.json).
```
cd server/src/Keysynth.Api
$env:ASPNETCORE_ENVIRONMENT="Development"; $env:KS_DEV_SERVE_FILES="1"; dotnet run
```
Listens on http://localhost:5020. With `KS_DEV_SERVE_FILES=1` sample files are served by the API itself (with
ranges) instead of `X-Accel-Redirect`; the index is `<SamplesRoot>/index.json` (`scripts/build_sample_index.py`).

## Configuration (environment)
| variable | meaning |
|---|---|
| `ConnectionStrings__DefaultConnection` | MySQL connection string (CI secret `DB_CONNECTION_STRING`) |
| `Mvv42Auth__JwksUrl`, `Mvv42Auth__CookieName`, `Mvv42Auth__AllowedOrigins__0` … | SDK options; production defaults (`http://mvv42-auth:8080/…`, `__Host-mvv42_at`, `https://mvv42.ru`) |
| `Keysynth__SamplesRoot` | samples dir with `index.json` (default `/data/samples`; container volume `/var/lib/keysynth:/data:ro`) |
| `KS_DEV_SERVE_FILES=1` | dev only: serve files directly |
| `ASPNETCORE_ENVIRONMENT` | `Development` enables CORS for `http://localhost:5173` |

## Deploy
`.github/workflows/backend.yml` (push to `main` touching `server/**`, or manual): tests → image
`ghcr.io/max31000/keysynth-api:{latest,sha-…}` → container `keysynth-api` on `127.0.0.1:5020`, network
`cashpulse-network`, health poll, rollback to the previous container.
