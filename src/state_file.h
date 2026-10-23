/*
 * state_file.h - writes the running game as a commander file the game's own
 * loader accepts (the format of L73038_MakeSaveData: the big space image and
 * the A6 globals, zero-run coded and encrypted), so any moment, in flight too,
 * can be reloaded or edited with tools/savetool.
 */
#ifndef STATE_FILE_H
#define STATE_FILE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Writes the state to `path`; false on failure */
bool state_file_write(const char *path);

/* Writes savs/SnapNN with the first free NN (Ctrl-S); the name goes in `name` */
bool state_file_snapshot(char *name, int size);

#ifdef __cplusplus
}
#endif

#endif /* STATE_FILE_H */
