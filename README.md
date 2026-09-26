# desktop

A Wayland desktop environment built from two forks and a thin layer of glue:

| Component | Repo | Role |
|---|---|---|
| Compositor | `~/src/umbriel` (fork of [noctalia-dev/umbriel](https://github.com/noctalia-dev/umbriel)) | Windows, layouts, effects, input, outputs, motion |
| Shell | `~/src/noctalia` (fork of [noctalia-dev/noctalia](https://github.com/noctalia-dev/noctalia)) | Bar, launcher, lock, notifications, settings, system services |
| Session | this repo | Session entry, systemd user units, portal routing, defaults |

This repo holds the design and the session glue. Code lives in the forks.

| Document | Covers |
|---|---|
| [docs/principles.md](docs/principles.md) | Code rules every change follows |
| [docs/architecture.md](docs/architecture.md) | Components, ownership, data flow, fork strategy |
| [docs/protocol.md](docs/protocol.md) | The private compositor/shell Wayland protocol |
| [docs/native-apis.md](docs/native-apis.md) | Every native API each module calls, with signatures |
| [docs/performance.md](docs/performance.md) | Budgets, baseline, and how to measure |
| [docs/standards.md](docs/standards.md) | Process split, monorepo, contract, boundary, pipeline |
| [docs/power.md](docs/power.md) | Display-on idle power program and measurement |
| [docs/gaps.md](docs/gaps.md) | What is missing, by owner and tier, and the order of work |
