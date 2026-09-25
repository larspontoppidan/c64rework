# Stage 4 review

The Stage 4 code starts as a copy of Stage 3. The purpose of Stage 4 is to
lift the code from looking like a converted C64 binary and from relying on the
C64 memory map, into a beautiful C++ game project with internal game state and
with a well designed internal structure.

Throughout Stage 4 the code must pass the play tests used in Stage 3, and Stage 4
adds a stricter lint check that enforces rules such as avoiding the mem bag.
Stage 4 can be seen as a purely mechanical exercise of maintaining successful
plays while burning down the lint findings, with Stage 4.5 further increasing
requirements of the code. Please read `docs/agents/STAGE4.md` for the details.
While these things are important, they are ultimately mechanical and the
worker agent will figure them out eventually.

Now, your job is to look at the code on a higher level. You must look at the
shape and feel of the refactored code as it's taking shape. You are the senior
architect, and your interest is the quality of the code and whether it matches
the spirit intended.

You must keep in mind what will happen with the code after C64Rework is done.
It will be remastered. The C64 chips will be removed and custom graphics and sound
routines will be put in place. The input handling and main loop will be improved
for the PC architecture. Maybe a SAVE/LOAD feature will be added in a new menu.
But, the game's wonderful internal logic will likely be preserved, as assets
and management around it will be upgraded. **The spirit of stage 4 is to lay the
ground work for a code base that can be remastered and extended effortlessly**.
And that means a code base that is:

- Modular with separation of concerns: sound, graphics, inputs, game-logic, assets, etc.
- Has well chosen function and variable names
- Has well designed internal APIs
- Is easily readable, with the caveat that the AtPc statements everywhere are not
very pretty. But these can be ignored because standalone export removes them.
Regard them as necessary scaffolding that gives us the ability to test the code
very strongly.

Stage 3 produced a code base that closely mirrors what was programmed in the 
original C64 game. That was the starting point. Stage 4 is about releasing us from
the shackles of that. I think the point is clear now.

Now for the practical matters.

The purpose of your review is NOT to repeat all the mechanical checks. The
worker agent will have done this.

The goal of the review is to generate these deliverables:

1. A brief evaluation of existing REVIEW.md findings if `Stage4/REVIEW.md`
exists. Go through them and if something is seriously missing or wrong, mention
it in the new review.

2. Identify show stoppers. Did the work slice introduce cheats or ugly hacks to
achieve lower lint scores. Remember that untouched Stage 3 code will of course
be not very pretty.

3. Suggest a reasonable chunk of work to tackle as the next slice. This may be
something that burns down lints, it may be a useful refactor, or it may be multiple
tasks. Don't make the slice too small.

Write a new: `Stage4/REVIEW.md` with the deliverables. Don't be afraid
to delete the existing content of the file.

## Hints

Things to remember checking and pointing out:

- All strings in the assets should be legible strings or chars, not hexadecimals.
This will most likely require a petscii to ascii conversion system. This can be done
compile-time in C++.
- Check that blobs of data are not just referred to in an anonymous way. A typical
example of this is the sprite installation in Stage 4.5. Make sure each sprite has a
sensible name.
