
# Checks and preparations before starting work in a game folder

Look at `./rework.toml` and verify that filenames look correct, `revm_root`
points to the intended framework, and `minimum_c64rework_version` equals the
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

If the build fails with CMake errors indicating the build folder had been 
run on another host, don't think twice about cleaning it out. Simply:

```bash
rm -rf build
```

If anything else fails in the preparations phase, abort the work and explain
to the user before continuing, unless instructed to try and fix problems 
yourself.
