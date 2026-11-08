/*
 * diag_upload.h - "Send diagnostics" (menu, Debug page; Android only).
 *
 * Only when the player presses the button: the log (this run's and the last
 * one's), the frame profile if it was on, and a description of the device
 * (made on the Java side) are zipped and sent to the upload server. The player
 * gets a short ID to quote in their GitHub issue. Nothing is sent otherwise.
 */
#ifndef DIAG_UPLOAD_H
#define DIAG_UPLOAD_H

#include <stdbool.h>

enum
{
	DIAG_IDLE,
	DIAG_SENDING,
	DIAG_SENT, /* diag_upload_message() = the ID */
	DIAG_FAILED /* diag_upload_message() = why */
};

/* false: not on this platform, or this build has no upload address */
bool diag_upload_available(void);
void diag_upload_start(void);
int diag_upload_state(void);
const char *diag_upload_message(void);

/* the frame profile, switched on from the menu (stays on until the game ends) */
void diag_profile_start(void);
bool diag_profile_on(void);

#endif /* DIAG_UPLOAD_H */
