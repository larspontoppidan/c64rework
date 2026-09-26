
This file is placed is the root folder of the C64 Rework framework. Read
README.md here for an overview of the project.

During the rework process of a C64 game, it should not be necessary to modify 
any files here. If you feel a feature is sorely missing in the framework or if
a bug has been identified, stop the rework process and present the case with
clear evidence to the user. 

**Don't modify files here in the process of reworking a game**.


## If instructed to work on REVM or the framework:

How to compile and test revm: ALWAYS use a game being reworked as the vehicle
to work on revm. Always use the game folder's `rework` script to build revm.
Having a game with an advanced state provides the best vehicle to work on revm,
as plays there can be used as test gates.

When modifying files in revm source files, remember to check if it affects the
hashes in standalone/OWNERSHIP.toml. From a game folder, run ./rework selftest
to verify.
