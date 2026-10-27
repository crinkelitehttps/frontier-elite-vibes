/*
 * gamepad_legacy.h - gamepad.cfg files written before the move to vibe-lib's
 * binding format ("format 2", marked in the first comment) are converted:
 * `manual <control>` becomes `in manual <control>`, `strafe left` and the
 * like become strafe-x / strafe-y, pointer-x / pointer-y go `in pointer`,
 * and each binding gets the lines for the VR controllers that used to be
 * read as an Xbox pad (the thumbsticks, grips, menu buttons, trackpad
 * and face buttons in the same places).
 */
#ifndef GAMEPAD_LEGACY_H
#define GAMEPAD_LEGACY_H

#include <string>

/* The file is in the old format (no format 2 mark) */
bool gamepad_text_is_legacy(const std::string &text);
/* The same bindings in the current format. The galaxy map's scrolling
 * (the default's `in map` lines) is added if the file has none. */
std::string gamepad_convert_legacy(const std::string &text);

#endif /* GAMEPAD_LEGACY_H */
