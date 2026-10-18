# Play checklist

Manual checks that a build is playable. Run them with the session report enabled:

```sh
tools/acre.py run -- --recomp-stats=work/logs/checklist.json
tools/acre.py roots --from work/logs/checklist.json   # after closing the game
```

A build passes when every row is OK and `roots --from` reports 0 new roots. If it adds
roots, run `tools/acre.py export && tools/acre.py build` and repeat the affected rows.

| # | Check | What "OK" means | Result |
|---|---|---|---|
| 1 | Window | Opens on the monitor set by `display_index` (primary by default) | |
| 2 | Controller | Every button and both sticks respond as on Switch | |
| 3 | Title menu | Menu shows; New Game starts | |
| 4 | Prologue | Plays through to the first fight without stalls | |
| 5 | First fight | Combat, camera and hit effects behave | |
| 6 | Legion | Summoning and controlling the Legion works | |
| 7 | Real-time cutscene | Plays at normal speed, audio in sync | |
| 8 | Save | Saving reports success | |
| 9 | Reload | Quit, start again, load the save; progress is there | |
| 10 | Report | `roots --from` adds 0 roots | |

With `fps60 = true` in `astralchain.ini` (restart after changing it), also check:

| # | Check | What "OK" means | Result |
|---|---|---|---|
| 11 | Cutscene timing | The same cutscene lasts the same at `fps60 = false` and `true` (within 2%; a phone stopwatch is enough) | |
| 12 | Game speed at 60 | Combat, Legion, jumps, dash, timers and the HUD counter run at normal speed | |
| 13 | Resolution | `resolution_scale = 2` looks at least as sharp as 1, with no bloom, depth of field or shadow artefacts | |
| 14 | Report | `roots --from` of the 60fps session adds 0 roots | |

Overlay:

| # | Check | What "OK" means | Result |
|---|---|---|---|
| 15 | Open and close | Insert and L3+R3 each open and close the overlay | |
| 16 | Game input blocked | With the overlay open the character does not react to the controller or keyboard; the game keeps running | |
| 17 | Nothing stuck | After closing, no button or key acts as if still held | |
| 18 | Controller navigation | Every tab and setting can be reached and changed with the controller | |
| 19 | Live settings | vsync, scaling filter, volume, fullscreen and the fps counter change at once | |
| 20 | Save | "Save to ini" writes the changed values and keeps the file's comments | |

Note anything odd under the table: graphical glitches, slow areas, crashes (with the
time), wrong buttons.
