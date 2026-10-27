/*
 * gamepad_legacy.cpp - see gamepad_legacy.h.
 */
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "gamepad_legacy.h"

namespace
{

/* The galaxy map scrolls with the arrow keys; written in the current
 * format (the converter adds the VR lines) */
const char MAP_LINES[] =
	"\n"
	"# ---- The galaxy map: the sticks scroll it (the arrow keys) instead of flying.\n"
	"# The right one moves the pointer, not the map, while the pointer is out.\n"
	"in map leftx none\n"
	"in map lefty none\n"
	"in map leftx- key left\n"
	"in map leftx+ key right\n"
	"in map lefty- key up\n"
	"in map lefty+ key down\n"
	"in map-nopointer rightx none\n"
	"in map-nopointer righty none\n"
	"in map-nopointer rightx- key left\n"
	"in map-nopointer rightx+ key right\n"
	"in map-nopointer righty- key up\n"
	"in map-nopointer righty+ key down\n";

/* A pad control and the VR controls that were read as it (gl_laser.c's
 * virtual pad): a hand's thumbstick, trigger, grip, menu button, face
 * buttons, and the trackpad as the d-pad (left) or face buttons (right) */
const struct
{
	const char *pad, *vr[2];
} VR_MAP[] = {
	{"a", {"right-a", "right-trackpad-down"}},
	{"b", {"right-b", "right-trackpad-right"}},
	{"x", {"left-x", "right-trackpad-left"}},
	{"y", {"left-y", "right-trackpad-up"}},
	{"back", {"left-menu", NULL}},
	{"start", {"right-menu", NULL}},
	{"leftstick", {"left-thumbstick", NULL}},
	{"rightstick", {"right-thumbstick", NULL}},
	{"leftshoulder", {"left-squeeze", NULL}},
	{"rightshoulder", {"right-squeeze", NULL}},
	{"dpup", {"left-trackpad-up", NULL}},
	{"dpdown", {"left-trackpad-down", NULL}},
	{"dpleft", {"left-trackpad-left", NULL}},
	{"dpright", {"left-trackpad-right", NULL}},
	{"leftx", {"left-thumbstick-x", NULL}},
	{"lefty", {"left-thumbstick-y", NULL}},
	{"rightx", {"right-thumbstick-x", NULL}},
	{"righty", {"right-thumbstick-y", NULL}},
	{"lefttrigger", {"left-trigger", NULL}},
	{"righttrigger", {"right-trigger", NULL}},
};

const char *const SETTINGS[] = {"deadzone", "threshold", "look-yaw", "look-pitch", "pointer-speed", "repeat"};

/* Names the old parser took in quotes */
const struct
{
	const char *quoted, *name;
} QUOTED[] = {{"left shift", "lshift"}, {"right shift", "rshift"}, {"left ctrl", "lctrl"}, {"left alt", "lalt"}};

std::string trim(const std::string &s)
{
	size_t a = s.find_first_not_of(" \t\r");
	if (a == std::string::npos)
		return "";
	return s.substr(a, s.find_last_not_of(" \t\r") + 1 - a);
}

/* Words; a quoted one may hold spaces and comes back as its alias */
std::vector<std::string> words(const std::string &body)
{
	std::vector<std::string> w;
	size_t p = 0;
	while (p < body.size())
	{
		while (p < body.size() && (body[p] == ' ' || body[p] == '\t' || body[p] == '\r'))
			p++;
		if (p >= body.size())
			break;
		std::string t;
		if (body[p] == '"')
		{
			size_t e = body.find('"', ++p);
			if (e == std::string::npos)
				e = body.size();
			t = body.substr(p, e - p);
			p = e < body.size() ? e + 1 : e;
			for (const auto &q : QUOTED)
				if (t == q.quoted)
					t = q.name;
		}
		else
		{
			size_t e = body.find_first_of(" \t\r", p);
			if (e == std::string::npos)
				e = body.size();
			t = body.substr(p, e - p);
			p = e;
		}
		w.push_back(t);
	}
	return w;
}

bool is_setting(const std::string &w)
{
	for (const char *s : SETTINGS)
		if (w == s)
			return true;
	return false;
}

/* One binding line: prefix ("shift1 " or "in map "), control, the rest */
std::string format(const std::string &prefix, const std::string &control, const std::string &rest,
				   const std::string &note)
{
	std::string left = prefix + control;
	if (left.size() < 18)
		left.append(18 - left.size(), ' ');
	else
		left += ' ';
	std::string s = left + rest;
	if (!note.empty())
	{
		if (s.size() < 36)
			s.append(36 - s.size(), ' ');
		s += " # " + note;
	}
	return s + "\n";
}

void blank(std::string &out)
{
	if (!out.empty() && out.compare(out.size() >= 2 ? out.size() - 2 : 0, 2, "\n\n") != 0)
		out += "\n";
}

} /* namespace */

bool gamepad_text_is_legacy(const std::string &text)
{
	return text.find("gamepad.cfg format 2") == std::string::npos;
}

std::string gamepad_convert_legacy(const std::string &text)
{
	std::string source = text;
	if (source.find("in map ") == std::string::npos)
		source += MAP_LINES;
	std::string pad_out, vr_out;
	size_t pos = 0;
	while (pos <= source.size())
	{
		size_t end = source.find('\n', pos);
		if (end == std::string::npos)
			end = source.size();
		std::string line = source.substr(pos, end - pos);
		bool last = end == source.size();
		pos = end + 1;
		if (last && line.empty())
			break;

		std::string note;
		std::string body = line;
		size_t hash = line.find('#');
		if (hash != std::string::npos)
		{
			body = line.substr(0, hash);
			note = trim(line.substr(hash + 1));
		}
		std::vector<std::string> w = words(body);
		if (w.empty())
		{
			if (hash == std::string::npos)
			{
				blank(pad_out);
				blank(vr_out);
			}
			else
				pad_out += trim(line) + "\n";
			continue;
		}
		if ((w.size() == 2 && is_setting(w[0])))
		{
			pad_out += line + "\n";
			continue;
		}

		size_t i = 0;
		std::string mode, layer;
		if (w[0] == "manual")
		{
			mode = "manual";
			i = 1;
		}
		else if (w[0] == "in" && w.size() > 2)
		{
			mode = w[1];
			i = 2;
		}
		if (i < w.size() && (w[i] == "shift1" || w[i] == "shift2") && i + 2 < w.size())
			layer = w[i++];
		if (i + 1 >= w.size())
		{
			pad_out += line + "\n"; /* the loader will say what is wrong */
			continue;
		}
		std::string control = w[i], action = w[i + 1], rest;
		std::vector<std::string> args(w.begin() + (long)i + 2, w.end());

		bool pointer = action == "pointer" || action == "pointer-x" || action == "pointer-y";
		if ((action == "pointer-x" || action == "pointer-y") && mode.empty())
			mode = "pointer";
		if (action == "strafe" && args.size() == 1)
		{
			const std::string &d = args[0];
			action = d == "left" || d == "right" ? "strafe-x" : "strafe-y";
			args.clear();
			if (d == "left" || d == "down")
				args.push_back("invert");
		}
		rest = action;
		for (const std::string &a : args)
			rest += " " + a;

		std::string prefix = !mode.empty() ? "in " + mode + " " : layer.empty() ? "" : layer + " ";
		pad_out += format(prefix, control, rest, note);
		if (pointer)
			continue; /* the motion controllers point */

		/* the same on the VR controllers */
		std::string base = control, suffix;
		if (base.size() > 1 && (base.back() == '-' || base.back() == '+'))
		{
			suffix = base.substr(base.size() - 1);
			base.resize(base.size() - 1);
		}
		for (const auto &m : VR_MAP)
			if (base == m.pad)
				for (const char *vr : m.vr)
					if (vr)
						vr_out += format(prefix, std::string(vr) + suffix, rest, note);
	}
	if (!pad_out.empty() && pad_out.back() != '\n')
		pad_out += "\n";
	return "# gamepad.cfg format 2: converted from the earlier format (see the default's header for the\n"
		   "# modes, the VR controls and the actions)\n\n" +
		   pad_out + "\n# ---- VR motion controllers: the same bindings on the headset's controllers\n" + vr_out;
}
