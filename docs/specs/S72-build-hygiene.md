# S72 — Build hygiene: fewer rebuilds, and sanitizer builds install

**Depends on.** S33. Touches headers across `editor/` and `engine/core/`; merge hotspot. Land fast and merge `main` often.

## 1. Goal
Editing a component header should not rebuild 521 editor files. That wait is paid by every engine change. Sanitizer Linux builds
should also produce an installed editor as normal builds do, so sanitizer MCP runs need no special case.

## 2. Requirements
- R1. Measure: for each `engine/core/component/*.h` and the top 20 most-included editor headers, how many translation units
  rebuild (ninja `-t` queries or depfiles). Save before and after tables in Deviations.
- R2. Cut the fan-out at its causes. Remove umbrella includes (e.g. a header that pulls in every component), use forward
  declarations, split headers, move code out of headers. Goal: editing a typical component header rebuilds at most 80 editor files;
  the worst case at most 150. If a limit proves impossible without harming the code, state why and give the achieved number.
- R3. No runtime change: no behaviour, no ABI of generated code, exported project template and prebuilt players still build. Each
  edited header still compiles alone (a header self-sufficiency check in `tools/`, run by check-changed).
- R4. Keep the portable C++20 subset; Windows and macOS code gets the same include fixes by careful reading, and the Mac build
  proves its side.
- R5. Sanitizer `linux` builds: `cmake --install --strip` at `tools/build.sh:413` installs nothing for `editor-address-undefined`.
  Find why, fix it, and make MCP tests able to run on that install. Strip is skipped in sanitizer builds.

## 3. Acceptance
- AC1. Before/after tables. A check in `tools/` fails if fan-out passes the new worst-case limit.
- AC2. `tools/build.sh linux`, `web`, `android`, unit tests, unity build, Mac editor build, all green.
- AC3. `SANITIZE=address,undefined tools/build.sh linux` then an MCP test file run against its install.
- AC4. `tools/check-changed` and docs (`docs/testing.md`).
