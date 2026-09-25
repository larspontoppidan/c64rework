
See README.md. See STANDALONE.md in revm folder.

## Hints for working on the code

View the following as hints, theory, explanations and suggestions on how to move
in a productive way with the project. But, instructions from the user win over
anything written here.

Focus on modifying files in `src/game`, this is the heart of the code. The other
folders should be viewed as library code. The work will most likely focus on 
improving the code to be less reliant on REVM and to be more standalone. To go
in this direction, consider building a dedicated visual pipeline suiting the 
game, while keeping the revm emulated VIC for game logic. At some point, perhaps
ditch the VIC entirely. Also consider lifting the game code away from the CIA 
chips and make a native main loop that looks more like a modern C++ game.

About sound, the original SID music provided by reSID is probably already really
great. The C64 had amazing music thanks to that SID chip. But now that we are 
not confined by C64 hardware, consider building a sound system that keep using
reSID for music but allow mixing in extra stuff like sound effects. Or you could
consider fancying up the SID output with more channels, stereo separation or 
reverb. Such an operation would require work in `src/resid` and that would be okay.
Beware that there is another, simpler, SID implementation inside REVM. It comes
from the Frodo V4 emulator REVM is built on. This implementatione does NOT sound
as good as reSID, so don't use its audio output. It may still be useful for pure
SID logic emulation or as a code example on how to emulate the SID.

## Main / Twin explanation

In comments and perhaps other places you will see a lot of mentions of Main / Twin.
These two were instrumental in the reverse engineering process that happened 
before and ultimately produced the standalone export you see here. The Main board 
refers to the machine run by the reverse engineered code, the `src/game` code, 
while Twin was a live oracle used in the process to keep Main honest. The Twin was 
a separate C64 board running the original 6510 code separately. The reverse 
engineering process aimed to replicate in Main exactly what the Twin was doing. 
Now, it turned out to be extremely hard to replicate the exact cycle level timing 
in Main independently, so the process leaned heavily on synchronization calls 
"*AtPc" lock stepping with things that happened at certain program counters in the
Twin. The idea was essentially to let Twin provide then "when", while Main provided
the "what". This was basically a crutch to allow the reverse engineering process 
to stay in lockstep and be verifiably honest.

Now, the standalone export here has been entirely stripped of the Twin (it used to
live inside REVM) and of the *AtPc synchronization calls, but you may see remnants.
But now you understand where it comes from and you understand that this can be 
fully ignored and you may delete all comments talking about the struggles to keep 
Main and Twin in sync. It's not a relevant topic anymore, the code is now 
standalone and should be transformed into something new, leaving behind the legacy
of where it came from!

