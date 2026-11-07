# Doriax vs JasperEngine: the comparison behind this program

Date: 2026-11-04. Doriax `main` at `3fb49fbba` (2026-11-03) against JasperEngine `main` at `b4f7816ea`.
Fork point: Doriax `29348d7b4` (2026-09-29, tag `doriax-base`). An earlier catch-up (`5d7d7c487`, 2026-10-24) had
already ported part of Doriax's work.

## Method

- All 319 Doriax commits since the fork, split into 24 domains, each read by an agent against JasperEngine's current
  code: status (missing, partial, already ported, solved differently, not applicable), evidence on both sides
  (path:line), impact, effort, worth.
- Head-to-head comparisons of final code where both engines changed a lot (rendering cost, shadows, probes and SSR,
  water, MCP, monetization, UI navigation, prefabs and scenes, editor workflow, memory and idle).
- 17 skeptic agents re-checked the result: every must/should finding (103 confirmed, 2 refuted, 6 unsettled by
  reading) and every bug fix claimed as not needed (88 confirmed, 4 overturned).
- Everything was static reading: nothing was built or run. Sizes are arithmetic from headers.

## Result

| | Commits |
|---|---|
| Already ported, solved differently, or not applicable | 144 |
| Missing or partial, must or should take | 84 |
| Missing or partial, optional | 80 |
| Docs, README, examples | 11 |

JasperEngine is ahead on directional shadows, ECS lookups, pipeline creation, prefabs, MCP (about 284 tools against
103), HTML UI navigation, buoyancy and water bodies, version control, typed math and save safety. Doriax is ahead on
rendering cost (mesh memory, sprite batching, redundant GPU state), a set of editor and export bug fixes, SSR under
probes, water reflections and boats, and a few editor conveniences.

The 84 items, the findings of our own and the optional items the owner chose to include were grouped into the 19
features of [LEDGER.md](LEDGER.md). The full per-commit ledger with evidence and verdicts is kept outside the
repository at `/var/lib/orquester/workspaces/jaspersito/dc-work/analysis/ledger-verified.json`.
