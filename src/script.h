/*
 * script.h - a small command language for starting the game in a given state
 * and driving it without a window: --script FILE, --do "cmd; cmd" and
 * --control FIFO (more commands while it runs). See script.c for the commands.
 */
#ifndef SCRIPT_H
#define SCRIPT_H

#ifdef __cplusplus
extern "C"
{
#endif

/* main.c: the command line options */
void script_add_file(const char *path);
void script_add_text(const char *commands);
void script_set_control(const char *fifo_path);

/* The game thread, once per host update: runs commands until one has to wait */
void script_poll(void);

#ifdef __cplusplus
}
#endif

#endif
