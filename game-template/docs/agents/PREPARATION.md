
# Checks and preparations before starting work in a game folder

Look at `./rework.toml` and verify that filenames look correct, `revm_root`
points to the intended framework, and `c64rework_version` equals the
framework `VERSION` exactly. C64 ROM dumps belong in the framework
`roms/` directory unless `rom_dir` overrides that.

Take a look at paths:

`./rework doctor`

Set up the game-local Python tools and verify the framework scripts:

```bash
./rework setup-venv
./rework selftest
```

Build revm:

`./rework build revm`

`./rework build` discards a CMake cache written at a different absolute path
(another host, container, or a moved folder) and reconfigures that target.
To wipe all generated build outputs:

```bash
./rework clean
```

If anything else fails in the preparations phase, abort the work and explain
to the user before continuing, unless instructed to try and fix problems 
yourself.
