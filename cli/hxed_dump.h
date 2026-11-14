#pragma once

// oshexed's command line (the old hxed_dump's commands): `oshexed --COMMAND ARGS` runs the command and exits without
// a window (frontend/main.cpp). One binary for the editor and the command line.

// argv[1] is the bare command, as in {"oshexed", "info", "FILE.hxf"}. Prints to stdout / stderr, returns the exit
// code.
int hxedDumpMain(int argc, char** argv);

// `--COMMAND args` (and `--dump WHAT args` = `WHAT args`). False for no arguments, a file to open, and the engine's
// own `--lumi-licenses`: those start the editor.
bool isCliCommand(int argc, char** argv);
// Runs a command line isCliCommand accepted (translated for hxedDumpMain).
int runCli(int argc, char** argv);
