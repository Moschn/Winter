Winter is a chess engine that relies on the UCI protocol to communicate with a GUI.

In order to run it on Linux, just compile it via "make" in the root directory and then run it from the root directory. Systems which do not have the newest generation of processors may lack access to the BMI2 instruction set, in this case compile with "make no_bmi" instead. If everything is working correctly entering "go depth 14" should result in precisely the following output with exception of the time:

go depth 14
info  depth 1 seldepth 1 time 0 nodes 21 score cp 23 pv e2e3
info  depth 2 seldepth 3 time 0 nodes 162 score cp 4 pv d2d4 d7d5
info  depth 3 seldepth 7 time 1 nodes 454 score cp 19 pv d2d4 d7d5 g1f3
info  depth 4 seldepth 10 time 5 nodes 1576 score cp 4 pv d2d4 d7d5 g1f3 g8f6
info  depth 5 seldepth 12 time 11 nodes 3679 score cp 14 pv d2d4 d7d5 g1f3 g8f6 b1c3
info  depth 6 seldepth 13 time 33 nodes 10271 score cp 4 pv d2d4 d7d5 g1f3 g8f6 b1c3 b8c6
info  depth 7 seldepth 19 time 61 nodes 43942 score cp 10 pv d2d4 d7d5 b1c3 g8f6 c1f4 g7g5 f4g5
info  depth 8 seldepth 19 time 94 nodes 89198 score cp 4 pv d2d4 d7d5 b1c3 g8f6 g1f3 b8c6 c1f4 c8f5
info  depth 9 seldepth 21 time 145 nodes 163684 score cp 15 pv d2d4 d7d5 b1c3 g8f6 g1f3 b8c6 c1f4 c8f5 e2e3
info  depth 10 seldepth 22 time 288 nodes 378670 score cp 6 pv d2d4 d7d5 b1c3 b8c6 e2e4 g8f6 f2f3 g7g6 e4e5 f6h5
info  depth 11 seldepth 24 time 528 nodes 743982 score cp 14 pv d2d4 d7d5 b1c3 g8f6 c1f4 b8c6 e2e3 a7a6 f1d3 g7g6 g1f3
info  depth 12 seldepth 24 time 1039 nodes 1535005 score cp 6 pv d2d4 d7d5 b1c3 g8f6 c1f4 c7c5 g1f3 c5d4 f3d4 b8c6 h2h3 d8a5
info  depth 13 seldepth 27 time 1997 nodes 3019184 score cp 13 pv d2d4 d7d5 b1c3 g8f6 c1f4 c7c5 g1f3 b8c6 e2e3 c5c4 f1e2 d8b6 d1c1
info  depth 14 seldepth 32 time 5921 nodes 8951954 score cp 7 pv d2d4 d7d5 c2c4 d5c4 e2e4 c7c6 f1c4 g8f6 c4d3 e7e5 d4e5 f6g4 b1c3 g4e5
bestmove d2d4