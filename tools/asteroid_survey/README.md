# Asteroid cap survey

`survey.py DIR` enters every system id listed in `DIR/ids.txt` (hex, one per line) with the script engine
(`enter ID`) and reads the asteroid cap (`peek b a6+13353`) after each, 40 per game launch. It writes
`DIR/caps.txt` (`ID CAP`; -1 = the batch's last system, whose read is lost when the game quits).
`DIR` must hold `savs/asteroid` (any save) and `glfrontier.cfg`; edit `B` for the binary. About 3 s per system.
Ids came from `FE2GalaxyViewer --catalog` over sectors -40..40 (48,339 systems). Results: docs/asteroid-spawning.md.
