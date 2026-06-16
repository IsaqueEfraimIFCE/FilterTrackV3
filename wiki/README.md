# FilterTrack Wiki

This directory is the LLM-maintained project wiki for FilterTrack.

The source-of-truth inputs used to build the first version are:

- [../llm-wiki.md](../llm-wiki.md) - wiki operating pattern.
- [../context.txt](../context.txt) - consolidated historical context.
- [../info/](../info/) - modular AI handoff notes.
- [../backend-fastapi/README.md](../backend-fastapi/README.md) - backend README.
- [../firmware/](../firmware/) - in-repo ESP-IDF firmware project.

Treat those inputs as raw sources and this directory as the compiled knowledge
layer. Future agents should read [index.md](index.md) first, then open the
topic pages that match the task.

## Core Pages

- [index.md](index.md) - content catalog and quick navigation.
- [schema.md](schema.md) - maintenance rules for this wiki.
- [sources.md](sources.md) - source register and conflict notes.
- [log.md](log.md) - chronological change log.
- [project-overview.md](project-overview.md) - product purpose and active system.

## Main Topic Pages

- [architecture.md](architecture.md) - end-to-end system architecture.
- [android-app.md](android-app.md) - Android shell, WebView UI, BLE bridge, local sessions.
- [android-play-internal-testing.md](android-play-internal-testing.md) - Play internal testing build and upload checklist.
- [ble-firmware-contract.md](ble-firmware-contract.md) - firmware-facing BLE protocol.
- [sensor-processing-flow.md](sensor-processing-flow.md) - distance filtering, velocity, flow.
- [sessions-data-contract.md](sessions-data-contract.md) - upload and storage contracts.
- [backend-fastapi.md](backend-fastapi.md) - FastAPI app, models, routes, config.
- [bi-dashboard.md](bi-dashboard.md) - BI dashboard, roles, proposals, exports.
- [deployment-operations.md](deployment-operations.md) - Fly.io deploy, validation, backups.
- [risks-open-items.md](risks-open-items.md) - risks, caveats, and pending decisions.
- [agent-workflows.md](agent-workflows.md) - task-specific workflow shortcuts.
