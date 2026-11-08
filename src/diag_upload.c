/*
 * diag_upload.c - see diag_upload.h. The zipping and the HTTPS request are
 * GLFrontierActivity.uploadDiagnostics (Java); this runs it on its own thread.
 */
#include <stdio.h>
#include <string.h>

#include <SDL.h>

#include "diag_upload.h"
#include "frame_prof.h"
#include "game_state.h"
#include "main.h"
#include "perf_log.h"

static SDL_atomic_t state; /* DIAG_* */
static char message[256];

int diag_upload_state(void)
{
	return SDL_AtomicGet(&state);
}

const char *diag_upload_message(void)
{
	return message;
}

bool diag_profile_on(void)
{
	return prof_on;
}

#ifdef ANDROID
#include <jni.h>

static char ext_dir[512];

static const char *external_dir(void)
{
	if (!ext_dir[0])
	{
		const char *d = SDL_AndroidGetExternalStoragePath();
		if (d)
			snprintf(ext_dir, sizeof ext_dir, "%s", d);
	}
	return ext_dir;
}

void diag_profile_start(void)
{
	char path[600];
	snprintf(path, sizeof path, "%s/frameprof.csv", external_dir());
	prof_start(path);
}

/* the activity's method, or 0; a thread of our own can't FindClass app classes,
 * so the class comes from the activity object */
static jmethodID upload_method(JNIEnv *env, jobject act, const char *name, const char *sig)
{
	jclass cls = (*env)->GetObjectClass(env, act);
	jmethodID m = (*env)->GetMethodID(env, cls, name, sig);
	(*env)->DeleteLocalRef(env, cls);
	if ((*env)->ExceptionCheck(env))
	{
		(*env)->ExceptionClear(env);
		return 0;
	}
	return m;
}

bool diag_upload_available(void)
{
	static int known = -1;
	if (known < 0)
	{
		JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
		jobject act = (jobject)SDL_AndroidGetActivity();
		known = 0;
		if (env && act)
		{
			jmethodID m = upload_method(env, act, "diagnosticsAvailable", "()Z");
			if (m)
			{
				known = (*env)->CallBooleanMethod(env, act, m) ? 1 : 0;
				if ((*env)->ExceptionCheck(env))
				{
					(*env)->ExceptionClear(env);
					known = 0;
				}
			}
			(*env)->DeleteLocalRef(env, act);
		}
	}
	return known == 1;
}

static int upload_thread(void *unused)
{
	(void)unused;
	const char *dir = external_dir();
	char info[512];
	snprintf(info, sizeof info, "build tag %s\nprofile %s\nscreen %s\n", game_build_tag(), prof_on ? "on" : "off",
			 game_screen_name(game_screen()));
	perf_summary("diagnostics upload");
	log_printf("diagnostics: sending\n");
	prof_flush();

	JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
	jobject act = (jobject)SDL_AndroidGetActivity();
	const char *result = "ERR the Java side is missing";
	jstring jres = 0;
	if (env && act)
	{
		jmethodID m = upload_method(env, act, "uploadDiagnostics", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
		if (m)
		{
			jstring jdir = (*env)->NewStringUTF(env, dir);
			jstring jinfo = (*env)->NewStringUTF(env, info);
			jres = (jstring)(*env)->CallObjectMethod(env, act, m, jdir, jinfo);
			if ((*env)->ExceptionCheck(env))
			{
				(*env)->ExceptionClear(env);
				jres = 0;
				result = "ERR the upload threw an exception";
			}
			(*env)->DeleteLocalRef(env, jdir);
			(*env)->DeleteLocalRef(env, jinfo);
		}
	}
	const char *s = jres ? (*env)->GetStringUTFChars(env, jres, 0) : result;
	/* "OK <id>" or "ERR <why>" */
	bool ok = strncmp(s, "OK ", 3) == 0;
	snprintf(message, sizeof message, "%s", ok ? s + 3 : (strncmp(s, "ERR ", 4) == 0 ? s + 4 : s));
	log_printf("diagnostics: %s %s\n", ok ? "sent, ID" : "failed:", message);
	if (jres)
	{
		(*env)->ReleaseStringUTFChars(env, jres, s);
		(*env)->DeleteLocalRef(env, jres);
	}
	if (act)
		(*env)->DeleteLocalRef(env, act);
	SDL_AtomicSet(&state, ok ? DIAG_SENT : DIAG_FAILED);
	return 0;
}

void diag_upload_start(void)
{
	if (!SDL_AtomicCAS(&state, DIAG_IDLE, DIAG_SENDING) && !SDL_AtomicCAS(&state, DIAG_SENT, DIAG_SENDING) &&
		!SDL_AtomicCAS(&state, DIAG_FAILED, DIAG_SENDING))
		return; /* one at a time */
	message[0] = '\0';
	SDL_Thread *t = SDL_CreateThread(upload_thread, "diag_upload", NULL);
	if (t)
		SDL_DetachThread(t);
	else
	{
		snprintf(message, sizeof message, "cannot start a thread");
		SDL_AtomicSet(&state, DIAG_FAILED);
	}
}

#else /* not Android: players attach glfrontier.log to the issue themselves */

bool diag_upload_available(void)
{
	return false;
}

void diag_upload_start(void)
{
}

void diag_profile_start(void)
{
	prof_start("frameprof.csv");
}

#endif
