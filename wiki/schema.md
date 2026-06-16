# Wiki Schema

This wiki follows the pattern in [../llm-wiki.md](../llm-wiki.md): raw sources
stay outside the wiki, while this directory contains the compiled, linked,
LLM-maintained synthesis.

## Directory Roles

- `context.txt` and `info/` are raw source inputs. Read them, but do not treat
  them as the navigable wiki.
- `wiki/` is the generated knowledge layer. Agents may edit it when ingesting new
  sources, answering a substantial question, or documenting a completed change.
- Source files in `app/`, `backend-fastapi/`, and `firmware/` are the
  implementation authority when docs and code disagree.

## Page Types

- Index pages list links and one-line summaries.
- Topic pages synthesize behavior across sources and code paths.
- Contract pages define interfaces, payloads, or operational agreements.
- Risk pages track caveats, conflicts, and decisions still needing attention.
- Log entries record changes chronologically.

## Source Priority

When facts conflict:

1. Current source code wins for implemented behavior.
2. `info/` wins over older `context.txt` unless code contradicts it.
3. `context.txt` is useful for history and deployment notes, but may be stale.
4. `backend-fastapi/README.md` is useful for backend expectations, but source code
   should be checked before changing behavior.

For firmware tasks, prefer `firmware/main/FilterTrackv3.c` plus the firmware
CMake, manifest, lockfile, and `sdkconfig`. Avoid ingesting `firmware/build/` or
`firmware/managed_components/` unless directly relevant.

## Maintenance Workflow

When adding a new source or completing a significant task:

1. Read [index.md](index.md) and the relevant topic pages.
2. Read the new source or changed code.
3. Update existing pages before creating new ones.
4. Add cross-links from affected pages.
5. Update [index.md](index.md) if a page is added or its purpose changes.
6. Append an entry to [log.md](log.md) using the standard heading format.
7. Record source conflicts or uncertain claims in [risks-open-items.md](risks-open-items.md).

## Log Format

Use parseable headings:

```markdown
## [YYYY-MM-DD] type | short title
```

Recommended `type` values:

- `ingest` - a new source was integrated.
- `update` - existing wiki pages changed after code or project work.
- `query` - an answered analysis was filed back into the wiki.
- `lint` - consistency or health-check pass.

## Writing Rules

- Keep secrets out of all wiki pages.
- Prefer relative links for repo files and wiki pages.
- Use ASCII text unless quoting an existing project term requires otherwise.
- State uncertainty explicitly; do not smooth over conflicting deployment facts.
- Keep pages task-useful for future agents, not promotional.
