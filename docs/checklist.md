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

Note anything odd under the table: graphical glitches, slow areas, crashes (with the
time), wrong buttons.
