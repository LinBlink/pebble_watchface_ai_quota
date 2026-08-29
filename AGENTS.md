# Repository Guidelines

## Project Structure & Module Organization

This repository builds the `AI Quota` Pebble Time watchface for the `basalt` platform. Watch-side C code lives in `src/c/main.c`. Phone-side JavaScript is authored in `src/pkjs-template/index.js`; do not edit generated files under `src/pkjs/`. Python utilities in `tools/` render credentials and optionally collect quota data. Images and fonts referenced by `package.json` live under `resources/`, while documentation screenshots belong in `assets/`. Pebble build artifacts are written to `build/`.

## Build, Test, and Development Commands

Run these commands from the repository root in an environment with the Pebble SDK installed (the documented setup uses WSL):

- `python tools/make_secrets.py` generates `src/pkjs/pebble-js-app.js` from the tracked template and local credentials.
- `pebble build` compiles the C app and bundles the phone companion into a `.pbw`.
- `pebble install --emulator basalt` installs the latest build in the basalt emulator.
- `pebble logs --emulator basalt` streams watch and companion logs for debugging.
- `python tools/quota_collector.py once --dump` performs a one-shot quota API diagnostic.

## Coding Style & Naming Conventions

Follow the existing style: two-space indentation in C and JavaScript, four spaces in Python, `snake_case` for C/Python functions, `camelCase` for JavaScript functions, and uppercase constants. Keep Pebble layout constants grouped and document non-obvious screen constraints. Prefer small helpers and explicit error logging. There is no configured formatter or linter, so keep changes consistent with neighboring code.

## Testing Guidelines

No automated test suite or coverage threshold is configured. Every change should at least pass `pebble build` and an emulator smoke test. Start logs before installation and confirm the `pkjs boot` line plus expected `sent:` messages. For layout checks, temporarily enable `DEMO_DATA` in `src/c/main.c`; do not commit that diagnostic change. Exercise settings through `pebble emu-app-config --emulator basalt` when modifying configuration behavior.

## Commit & Pull Request Guidelines

Recent commits use short, imperative summaries such as `Add README...` and `Buzz once...`. Keep each commit focused and explain behavior, not file mechanics. Pull requests should summarize user-visible effects, list verification commands, link related issues, and include an emulator screenshot for visual changes. Call out credential-flow or persistence-key changes explicitly.

## Security & Generated Files

Never commit `config.json`, `.env` files, credentials, generated `src/pkjs/`, or `.pbw` artifacts. Generated companion JavaScript contains live tokens. Make changes in `src/pkjs-template/index.js`, rerun the generator locally, and inspect staged files before every commit.
