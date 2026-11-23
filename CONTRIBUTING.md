# Contributing to JasperEngine

- **Bugs** go to the issue tracker, with the steps that reproduce them and the platform and graphics
  backend.
- **Pull requests** follow `.github/pull_request_template.md` and `AGENTS.md`, which holds the rules
  for engine contributors, human or agent.
- **`Check`** must be green on a PR's latest commit before it can merge; merges go through the merge
  queue. `docs/testing.md` ("CI") says what each lane runs and how to reproduce it locally.
- **Builds** run inside the Docker images under `docker/`: `tools/build.sh linux` builds the editor,
  and `tools/build.sh` without arguments lists the other targets.
- **Releases** (nightly and stable) are unsigned: macOS asks for right-click, Open, the first time;
  Windows SmartScreen for More info, Run anyway.
