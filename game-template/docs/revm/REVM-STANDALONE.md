# revm-standalone runtime ownership

**revm-standalone** is a slimmed down version of revm intended for the 
standalone export process. The following information is relevant if changes
are done to `src/revm`. In some cases they should propagate to 
`src/revm-standalone`. The ownership of the files there have explicit
categorization and there are tools to audit status. The files (apart from
the manifest itself) fall in one of three explicit ownership classes:

- **Mirrored** files must be byte-identical to the corresponding `src/revm`
  file. The manifest supports directory prefixes so new files in a shared
  runtime area are checked automatically.
- **Forked** files may differ, but record the SHA-256 of the upstream bytes
  from which the fork was made and a reason for the divergence. If upstream
  changes, the audit fails until the fork is consciously refreshed or its
  ownership is changed.
- **Standalone-only** files have no upstream counterpart. The audit rejects a
  same-named file appearing in `src/revm`.

From a game root (after `./rework setup-venv`):

```sh
./rework selftest
```

`selftest` runs the ownership-auditor fixture and a live ownership audit of
`src/revm-standalone`, along with the other tool fixtures. `./rework export`
repeats the live ownership audit as preflight.

The normal repair direction is to fix shared behavior in `src/revm`, refresh
mirrored files, and update a fork only when the divergence is intentional.
Standalone orchestration and pruning belong in `src/revm-standalone`. A fork
entry is temporary only when its reason says so; removing the file also means
removing its manifest entry.
