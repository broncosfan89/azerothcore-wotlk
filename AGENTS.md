# AGENTS.md

## Working style
- One command at a time.
- Keep diffs small. After changes: show `git diff` and build `worldserver`.

## Project constraints
- Server-side only unless explicitly requested: do NOT modify client MPQ/DBC.
- Prefer C++ scripts/modules over DB-heavy changes, but DB migrations are OK.

## Client patch workflow (only when explicitly requested)
- If a task includes DBC-impacting changes, rebuild the client patch before finishing:
  `powershell -ExecutionPolicy Bypass -File tools/build_item_patch.ps1 -PatchInputInPlace`
- Output patch to copy into WoW client Data folder:
  `tools/client_patch/patch-Z.MPQ`

## Build (Windows / PowerShell)
- Build output dir: D:\azerothcore-wotlk-playerbot\build\bin\Release
- Build command (run from build folder): cmake --build . --config Release --target worldserver

## Databases (update if different)
- World DB: acore_world
- Characters DB: acore_characters
- Auth DB: acore_auth

## DB rules
- Never print credentials.
- All DB changes must be SQL migration files committed under `data/sql/custom/`.
- After applying migrations, run a quick verification query.
