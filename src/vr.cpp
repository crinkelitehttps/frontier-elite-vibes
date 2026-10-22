/*
 * vr.cpp - see vr.h. Adapted from the F-19 port's OpenXR output.
 */
#include "vr.h"

#ifdef GLF_VR /* (CMake option: desktop builds) */

#include "gl/gl_api.h"

#ifdef _WIN32
/* Windows: Direct3D 11 swapchains (the WMR runtime has no OpenGL). Each
 * eye is drawn with GL into a GL framebuffer, blitted (flipped: D3D is top
 * down) into a D3D11 texture shared with GL through WGL_NV_DX_interop2,
 * then copied into the runtime's swapchain image. */
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#elif defined(ANDROID)
/* Android (Quest): the runtime shares the EGL context SDL made, with
 * OpenGL ES swapchains */
#include <EGL/egl.h>
#include <SDL.h>
#include <jni.h>
#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#else
#include <X11/Xlib.h>
#include <GL/glx.h>
#define XR_USE_PLATFORM_XLIB
#define XR_USE_GRAPHICS_API_OPENGL
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <type_traits>
#include <vector>

extern "C" void log_printf(const char *fmt, ...);

bool vr_enabled = false;
int vr_resolution = 100;

namespace
{

#ifdef _WIN32
constexpr int64_t kFormatSrgb = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, kFormatRgba = DXGI_FORMAT_R8G8B8A8_UNORM;

/* WGL_NV_DX_interop2 (wglext.h) */
constexpr GLenum kWglAccessWriteDiscardNV = 0x0002;
using PFNWGLDXOPENDEVICENVPROC = HANDLE(WINAPI *)(void *dx_device);
using PFNWGLDXCLOSEDEVICENVPROC = BOOL(WINAPI *)(HANDLE device);
using PFNWGLDXREGISTEROBJECTNVPROC = HANDLE(WINAPI *)(HANDLE device, void *dx_object, GLuint name, GLenum type,
													   GLenum access);
using PFNWGLDXUNREGISTEROBJECTNVPROC = BOOL(WINAPI *)(HANDLE device, HANDLE object);
using PFNWGLDXLOCKOBJECTSNVPROC = BOOL(WINAPI *)(HANDLE device, GLint count, HANDLE *objects);
using PFNWGLDXUNLOCKOBJECTSNVPROC = PFNWGLDXLOCKOBJECTSNVPROC;
struct WglInterop
{
	PFNWGLDXOPENDEVICENVPROC OpenDevice = nullptr;
	PFNWGLDXCLOSEDEVICENVPROC CloseDevice = nullptr;
	PFNWGLDXREGISTEROBJECTNVPROC RegisterObject = nullptr;
	PFNWGLDXUNREGISTEROBJECTNVPROC UnregisterObject = nullptr;
	PFNWGLDXLOCKOBJECTSNVPROC LockObjects = nullptr;
	PFNWGLDXUNLOCKOBJECTSNVPROC UnlockObjects = nullptr;
	bool load()
	{
		auto get = [](auto &fn, const char *name) {
			fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(
				reinterpret_cast<void *>(wglGetProcAddress(name)));
			return fn != nullptr;
		};
		return get(OpenDevice, "wglDXOpenDeviceNV") && get(CloseDevice, "wglDXCloseDeviceNV") &&
			   get(RegisterObject, "wglDXRegisterObjectNV") && get(UnregisterObject, "wglDXUnregisterObjectNV") &&
			   get(LockObjects, "wglDXLockObjectsNV") && get(UnlockObjects, "wglDXUnlockObjectsNV");
	}
};

template <class T> void com_release(T *&p)
{
	if (p)
		p->Release();
	p = nullptr;
}
#else
constexpr int64_t kFormatSrgb = 0x8C43 /* GL_SRGB8_ALPHA8 */, kFormatRgba = 0x8058 /* GL_RGBA8 */;
#ifdef ANDROID
using SwapchainImage = XrSwapchainImageOpenGLESKHR;
constexpr XrStructureType kSwapchainImageType = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
#else
using SwapchainImage = XrSwapchainImageOpenGLKHR;
constexpr XrStructureType kSwapchainImageType = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
#endif
#endif

/* Head positions farther than this from the seat are tracking failures */
constexpr float kMaxHeadOffsetM = 1.0f;

char status[320] = "Off";

bool check(XrInstance inst, XrResult r, const char *what)
{
	if (XR_SUCCEEDED(r))
		return true;
	char s[XR_MAX_RESULT_STRING_SIZE] = "?";
	if (inst)
		xrResultToString(inst, r, s);
	log_printf("openxr: %s failed: %s (%d)\n", what, s, int(r));
	snprintf(status, sizeof status, "%s failed: %s", what, s);
	return false;
}

struct Swapchain
{
	XrSwapchain handle = XR_NULL_HANDLE;
	int w = 0, h = 0;
	uint32_t index = 0;
	GLuint stencil = 0; /* the cockpit's turned passes need a stencil buffer */
#ifdef _WIN32
	std::vector<ID3D11Texture2D *> images; /* the runtime's */
	GLuint render_fbo = 0, render_tex = 0; /* what GL draws into */
	ID3D11Texture2D *shared = nullptr;     /* GL's view of it: interop_tex */
	GLuint interop_tex = 0, interop_fbo = 0;
	HANDLE interop = nullptr;
#else
	std::vector<GLuint> fbos; /* one per swapchain image */
#endif
};

struct Vr
{
	XrInstance instance = XR_NULL_HANDLE;
	XrSystemId system = XR_NULL_SYSTEM_ID;
	XrSession session = XR_NULL_HANDLE;
	bool running = false, lost = false;
	/* base: the runtime's LOCAL space; play: LOCAL moved to the recentred
	 * eye point (yaw only), which all drawing uses */
	XrSpace base_space = XR_NULL_HANDLE, play_space = XR_NULL_HANDLE, view_space = XR_NULL_HANDLE;
	bool recenter_pending = true;
	Swapchain eyes[2], screen;
	XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
	XrFrameState frame{XR_TYPE_FRAME_STATE};
	bool in_frame = false, views_valid = false, drew_eyes = false, drew_screen = false;
	bool position_rejected = false;
	bool log = std::getenv("GLF_XR_LOG") != nullptr; /* print the eye pose twice a second */
	XrTime last_log = 0;
	int frames_since_log = 0;
	/* Motion controllers: each action has both hands as subaction paths */
	bool focused = false;
	XrActionSet action_set = XR_NULL_HANDLE;
	XrPath hand_paths[2] = {XR_NULL_PATH, XR_NULL_PATH};
	XrAction aim = XR_NULL_HANDLE, trigger = XR_NULL_HANDLE, squeeze = XR_NULL_HANDLE, stick = XR_NULL_HANDLE,
			 stick_click = XR_NULL_HANDLE, pad = XR_NULL_HANDLE, pad_click = XR_NULL_HANDLE, menu = XR_NULL_HANDLE,
			 lower = XR_NULL_HANDLE, upper = XR_NULL_HANDLE, haptic = XR_NULL_HANDLE;
	XrSpace aim_space[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
	VrHand hands[2] = {};
	bool hand_announced[2] = {};
	bool drew_overlay = false;

#ifdef _WIN32
	ID3D11Device *device = nullptr;
	ID3D11DeviceContext *context = nullptr;
	WglInterop wgl;
	HANDLE interop_device = nullptr;
	bool make_device();
#endif

	bool init(float resolution_scale);
	~Vr();
	bool make_swapchain(Swapchain &sc, int64_t format, int w, int h);
	bool make_play_space(const XrPosef &pose);
	void try_recenter(XrTime t);
	bool make_actions();
	void read_hands();
	GLuint acquire(Swapchain &sc);
	void release(Swapchain &sc);
	void poll();
	bool begin_frame();
	void end_frame();
};

Vr *vr;           /* while VR is on */
bool vr_failed;   /* the last start failed: not retried until vr_enabled changes */

#ifdef _WIN32
/* The D3D11 device on the adapter the runtime drives the headset with, and
 * GL's access to it */
bool Vr::make_device()
{
	PFN_xrGetD3D11GraphicsRequirementsKHR get_reqs = nullptr;
	xrGetInstanceProcAddr(instance, "xrGetD3D11GraphicsRequirementsKHR",
						  reinterpret_cast<PFN_xrVoidFunction *>(&get_reqs));
	XrGraphicsRequirementsD3D11KHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
	if (!get_reqs || !check(instance, get_reqs(instance, system, &reqs), "xrGetD3D11GraphicsRequirementsKHR"))
		return false;
	IDXGIFactory1 *factory = nullptr;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))))
	{
		log_printf("openxr: CreateDXGIFactory1 failed\n");
		snprintf(status, sizeof status, "CreateDXGIFactory1 failed");
		return false;
	}
	IDXGIAdapter1 *adapter = nullptr;
	for (UINT i = 0; factory->EnumAdapters1(i, &adapter) == S_OK; i++)
	{
		DXGI_ADAPTER_DESC1 desc;
		if (SUCCEEDED(adapter->GetDesc1(&desc)) && !std::memcmp(&desc.AdapterLuid, &reqs.adapterLuid, sizeof(LUID)))
		{
			char name[128];
			WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof name, nullptr, nullptr);
			log_printf("openxr: headset GPU: %s\n", name);
			break;
		}
		com_release(adapter);
	}
	com_release(factory);
	const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
										D3D_FEATURE_LEVEL_10_0};
	D3D_FEATURE_LEVEL got{};
	HRESULT hr = D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
								   levels, UINT(std::size(levels)), D3D11_SDK_VERSION, &device, &got, &context);
	com_release(adapter);
	if (FAILED(hr) || got < reqs.minFeatureLevel)
	{
		log_printf("openxr: cannot create a Direct3D 11 device (0x%08lx)\n", (unsigned long)hr);
		snprintf(status, sizeof status, "Cannot create a Direct3D 11 device");
		return false;
	}
	if (!wgl.load())
	{
		log_printf("openxr: the OpenGL driver lacks WGL_NV_DX_interop2 (needed to draw for Direct3D)\n");
		snprintf(status, sizeof status, "The OpenGL driver lacks WGL_NV_DX_interop2");
		return false;
	}
	interop_device = wgl.OpenDevice(device);
	if (!interop_device)
	{
		log_printf("openxr: wglDXOpenDeviceNV failed: OpenGL is probably on a different GPU from the headset "
				   "(set GLFrontier to the high-performance GPU in Windows graphics settings)\n");
		snprintf(status, sizeof status, "OpenGL is on a different GPU from the headset");
		return false;
	}
	return true;
}
#endif

/* A depth + stencil renderbuffer of the swapchain's size, on the bound
 * framebuffer */
static void attach_stencil(Swapchain &sc)
{
	if (!sc.stencil)
	{
		glGenRenderbuffers(1, &sc.stencil);
		glBindRenderbuffer(GL_RENDERBUFFER, sc.stencil);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, sc.w, sc.h);
		glBindRenderbuffer(GL_RENDERBUFFER, 0);
	}
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, sc.stencil);
}

bool Vr::make_swapchain(Swapchain &sc, int64_t format, int w, int h)
{
	XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
	ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
	ci.format = format;
	ci.sampleCount = 1;
	ci.width = uint32_t(w);
	ci.height = uint32_t(h);
	ci.faceCount = 1;
	ci.arraySize = 1;
	ci.mipCount = 1;
	if (!check(instance, xrCreateSwapchain(session, &ci, &sc.handle), "xrCreateSwapchain"))
		return false;
	sc.w = w;
	sc.h = h;
	uint32_t n = 0;
	xrEnumerateSwapchainImages(sc.handle, 0, &n, nullptr);
#ifdef _WIN32
	std::vector<XrSwapchainImageD3D11KHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
	if (!check(instance,
			   xrEnumerateSwapchainImages(sc.handle, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader *>(imgs.data())),
			   "xrEnumerateSwapchainImages"))
		return false;
	for (auto &img : imgs)
		sc.images.push_back(img.texture);
	/* Same size, and a format in the swapchain's typeless group, so that
	 * CopyResource copies the bytes as they are */
	D3D11_TEXTURE2D_DESC td{};
	td.Width = UINT(w);
	td.Height = UINT(h);
	td.MipLevels = td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	if (FAILED(device->CreateTexture2D(&td, nullptr, &sc.shared)))
	{
		log_printf("openxr: cannot create the shared texture\n");
		return false;
	}
	glGenTextures(1, &sc.interop_tex);
	sc.interop = wgl.RegisterObject(interop_device, sc.shared, sc.interop_tex, GL_TEXTURE_2D, kWglAccessWriteDiscardNV);
	if (!sc.interop)
	{
		log_printf("openxr: wglDXRegisterObjectNV failed\n");
		return false;
	}
	glGenTextures(1, &sc.render_tex);
	glBindTexture(GL_TEXTURE_2D, sc.render_tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glBindTexture(GL_TEXTURE_2D, 0);
	auto make_fbo = [&](GLuint tex, GLuint &fbo, bool stencil) {
		glGenFramebuffers(1, &fbo);
		glBindFramebuffer(GL_FRAMEBUFFER, fbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
		if (stencil)
			attach_stencil(sc);
		return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
	};
	bool ok = make_fbo(sc.render_tex, sc.render_fbo, true);
	/* the interop texture is only usable while locked */
	wgl.LockObjects(interop_device, 1, &sc.interop);
	ok = make_fbo(sc.interop_tex, sc.interop_fbo, false) && ok;
	wgl.UnlockObjects(interop_device, 1, &sc.interop);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	if (!ok)
		log_printf("openxr: swapchain framebuffer incomplete\n");
	return ok;
#else
	std::vector<SwapchainImage> imgs(n, {kSwapchainImageType});
	if (!check(instance,
			   xrEnumerateSwapchainImages(sc.handle, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader *>(imgs.data())),
			   "xrEnumerateSwapchainImages"))
		return false;
	for (auto &img : imgs)
	{
		GLuint fbo = 0;
		glGenFramebuffers(1, &fbo);
		glBindFramebuffer(GL_FRAMEBUFFER, fbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, img.image, 0);
		attach_stencil(sc);
		sc.fbos.push_back(fbo);
		if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		{
			log_printf("openxr: swapchain framebuffer incomplete\n");
			glBindFramebuffer(GL_FRAMEBUFFER, 0);
			return false;
		}
	}
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	return true;
#endif
}

/* The controllers' actions, suggested for the common controllers (the
 * runtime picks the profile that matches what is in the hands) */
bool Vr::make_actions()
{
	XrActionSetCreateInfo asi{XR_TYPE_ACTION_SET_CREATE_INFO};
	std::strcpy(asi.actionSetName, "controls");
	std::strcpy(asi.localizedActionSetName, "Controls");
	if (!check(instance, xrCreateActionSet(instance, &asi, &action_set), "xrCreateActionSet"))
		return false;
	xrStringToPath(instance, "/user/hand/left", &hand_paths[0]);
	xrStringToPath(instance, "/user/hand/right", &hand_paths[1]);
	auto make = [&](XrAction &a, const char *name, const char *label, XrActionType type) {
		XrActionCreateInfo ai{XR_TYPE_ACTION_CREATE_INFO};
		std::strcpy(ai.actionName, name);
		std::strcpy(ai.localizedActionName, label);
		ai.actionType = type;
		ai.countSubactionPaths = 2;
		ai.subactionPaths = hand_paths;
		return check(instance, xrCreateAction(action_set, &ai, &a), "xrCreateAction");
	};
	if (!make(aim, "aim", "Pointer", XR_ACTION_TYPE_POSE_INPUT) ||
		!make(trigger, "trigger", "Trigger", XR_ACTION_TYPE_FLOAT_INPUT) ||
		!make(squeeze, "grip", "Grip", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
		!make(stick, "thumbstick", "Thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT) ||
		!make(stick_click, "thumbstick_click", "Thumbstick click", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
		!make(pad, "trackpad", "Trackpad", XR_ACTION_TYPE_VECTOR2F_INPUT) ||
		!make(pad_click, "trackpad_click", "Trackpad click", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
		!make(menu, "menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
		!make(lower, "lower_button", "Lower face button (A / X)", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
		!make(upper, "upper_button", "Upper face button (B / Y)", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
		!make(haptic, "haptic", "Buzz", XR_ACTION_TYPE_VIBRATION_OUTPUT))
		return false;

	/* Inputs: "both/..." on either hand, "left/..." or "right/..." on one;
	 * "haptic" is the output.
	 * Floats bound to buttons are thresholded by the runtime and the other
	 * way round give 0 or 1. */
	struct Input
	{
		XrAction action;
		const char *input;
	};
	struct Profile
	{
		const char *path;
		std::vector<Input> inputs;
	};
	const Profile profiles[] = {
		{"microsoft/motion_controller", /* WMR */
		 {{aim, "both/aim/pose"},
		  {trigger, "both/trigger/value"},
		  {squeeze, "both/squeeze/click"},
		  {stick, "both/thumbstick"},
		  {stick_click, "both/thumbstick/click"},
		  {pad, "both/trackpad"},
		  {pad_click, "both/trackpad/click"},
		  {menu, "both/menu/click"},
		  {haptic, "both/haptic"}}},
		{"oculus/touch_controller", /* no right menu (the system button) */
		 {{aim, "both/aim/pose"},
		  {trigger, "both/trigger/value"},
		  {squeeze, "both/squeeze/value"},
		  {stick, "both/thumbstick"},
		  {stick_click, "both/thumbstick/click"},
		  {menu, "left/menu/click"},
		  {lower, "left/x/click"},
		  {upper, "left/y/click"},
		  {lower, "right/a/click"},
		  {upper, "right/b/click"},
		  {haptic, "both/haptic"}}},
		{"valve/index_controller", /* no menu */
		 {{aim, "both/aim/pose"},
		  {trigger, "both/trigger/value"},
		  {squeeze, "both/squeeze/value"},
		  {stick, "both/thumbstick"},
		  {stick_click, "both/thumbstick/click"},
		  {lower, "both/a/click"},
		  {upper, "both/b/click"},
		  {haptic, "both/haptic"}}},
		{"htc/vive_controller", /* no thumbstick */
		 {{aim, "both/aim/pose"},
		  {trigger, "both/trigger/value"},
		  {squeeze, "both/squeeze/click"},
		  {pad, "both/trackpad"},
		  {pad_click, "both/trackpad/click"},
		  {menu, "both/menu/click"},
		  {haptic, "both/haptic"}}},
		{"khr/simple_controller",
		 {{aim, "both/aim/pose"}, {trigger, "both/select/click"}, {menu, "both/menu/click"}}},
	};
	int suggested = 0;
	for (const Profile &prof : profiles)
	{
		std::vector<XrActionSuggestedBinding> sb;
		for (const Input &in : prof.inputs)
		{
			const char *slash = std::strchr(in.input, '/');
			bool both = !std::strncmp(in.input, "both/", 5);
			for (const char *hand : {"left", "right"})
			{
				if (!both && std::strncmp(in.input, hand, std::strlen(hand)))
					continue;
				char path[128];
				snprintf(path, sizeof path, "/user/hand/%s/%s/%s", hand,
						 std::strcmp(slash + 1, "haptic") ? "input" : "output", slash + 1);
				XrPath xp;
				xrStringToPath(instance, path, &xp);
				sb.push_back({in.action, xp});
			}
		}
		XrInteractionProfileSuggestedBinding isb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
		char path[128];
		snprintf(path, sizeof path, "/interaction_profiles/%s", prof.path);
		xrStringToPath(instance, path, &isb.interactionProfile);
		isb.countSuggestedBindings = uint32_t(sb.size());
		isb.suggestedBindings = sb.data();
		if (check(instance, xrSuggestInteractionProfileBindings(instance, &isb), prof.path))
			suggested++;
	}
	if (!suggested)
		return false;
	for (int h = 0; h < 2; h++)
	{
		XrActionSpaceCreateInfo si{XR_TYPE_ACTION_SPACE_CREATE_INFO};
		si.action = aim;
		si.subactionPath = hand_paths[h];
		si.poseInActionSpace.orientation.w = 1;
		if (!check(instance, xrCreateActionSpace(session, &si, &aim_space[h]), "xrCreateActionSpace"))
			return false;
	}
	XrSessionActionSetsAttachInfo at{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
	at.countActionSets = 1;
	at.actionSets = &action_set;
	return check(instance, xrAttachSessionActionSets(session, &at), "xrAttachSessionActionSets");
}

/* The controllers at the frame's display time, in the play space */
void Vr::read_hands()
{
	for (VrHand &h : hands)
		h = {};
	if (action_set && focused)
	{
		XrActiveActionSet active{action_set, XR_NULL_PATH};
		XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO};
		si.countActiveActionSets = 1;
		si.activeActionSets = &active;
		if (XR_SUCCEEDED(xrSyncActions(session, &si)))
			for (int i = 0; i < 2; i++)
			{
				VrHand &h = hands[i];
				XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
				gi.subactionPath = hand_paths[i];
				auto get_bool = [&](XrAction a, bool &v) {
					gi.action = a;
					XrActionStateBoolean st{XR_TYPE_ACTION_STATE_BOOLEAN};
					if (XR_SUCCEEDED(xrGetActionStateBoolean(session, &gi, &st)) && st.isActive)
					{
						v = st.currentState;
						h.connected = true;
					}
				};
				auto get_vec = [&](XrAction a, float *v) {
					gi.action = a;
					XrActionStateVector2f st{XR_TYPE_ACTION_STATE_VECTOR2F};
					if (XR_SUCCEEDED(xrGetActionStateVector2f(session, &gi, &st)) && st.isActive)
					{
						v[0] = st.currentState.x;
						v[1] = st.currentState.y;
						h.connected = true;
					}
				};
				gi.action = trigger;
				XrActionStateFloat tr{XR_TYPE_ACTION_STATE_FLOAT};
				if (XR_SUCCEEDED(xrGetActionStateFloat(session, &gi, &tr)) && tr.isActive)
				{
					h.trigger = tr.currentState;
					h.connected = true;
				}
				get_bool(squeeze, h.squeeze);
				get_vec(stick, h.stick);
				get_bool(stick_click, h.stick_click);
				get_vec(pad, h.pad);
				get_bool(pad_click, h.pad_click);
				get_bool(menu, h.menu);
				get_bool(lower, h.lower);
				get_bool(upper, h.upper);

				XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
				const XrSpaceLocationFlags valid =
					XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
				if (!XR_SUCCEEDED(xrLocateSpace(aim_space[i], play_space, frame.predictedDisplayTime, &loc)) ||
					(loc.locationFlags & valid) != valid)
					continue;
				const XrQuaternionf &q = loc.pose.orientation;
				h.pos[0] = loc.pose.position.x;
				h.pos[1] = loc.pose.position.y;
				h.pos[2] = loc.pose.position.z;
				/* its -z */
				h.dir[0] = -2 * (q.x * q.z + q.y * q.w);
				h.dir[1] = -2 * (q.y * q.z - q.x * q.w);
				h.dir[2] = -(1 - 2 * (q.x * q.x + q.y * q.y));
				/* its +y */
				h.up[0] = 2 * (q.x * q.y - q.z * q.w);
				h.up[1] = 1 - 2 * (q.x * q.x + q.z * q.z);
				h.up[2] = 2 * (q.y * q.z + q.x * q.w);
				h.active = std::isfinite(h.pos[0] + h.pos[1] + h.pos[2] + h.dir[0] + h.dir[1] + h.dir[2]);
			}
	}
	for (int i = 0; i < 2; i++)
		if (hands[i].connected != hand_announced[i])
		{
			log_printf("openxr: %s controller %s\n", i ? "right" : "left", hands[i].connected ? "active" : "gone");
			hand_announced[i] = hands[i].connected;
		}
}

bool Vr::make_play_space(const XrPosef &pose)
{
	if (play_space)
		xrDestroySpace(play_space);
	play_space = XR_NULL_HANDLE;
	XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
	rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	rs.poseInReferenceSpace = pose;
	return check(instance, xrCreateReferenceSpace(session, &rs, &play_space), "xrCreateReferenceSpace");
}

/* Puts the play space's origin at the head, facing where it faces (yaw
 * only, so the horizon stays level). Stays pending until the head is
 * tracked. */
void Vr::try_recenter(XrTime t)
{
	XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
	if (!XR_SUCCEEDED(xrLocateSpace(view_space, base_space, t, &loc)) ||
		!(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
		return;
	const XrQuaternionf &q = loc.pose.orientation;
	/* forward (-z) in LOCAL space, projected onto the floor */
	float fx = -2 * (q.x * q.z + q.y * q.w), fz = -(1 - 2 * (q.x * q.x + q.y * q.y));
	float yaw = std::atan2(-fx, -fz);
	XrPosef pose{};
	pose.orientation = {0, std::sin(yaw / 2), 0, std::cos(yaw / 2)};
	if (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
		pose.position = loc.pose.position;
	if (make_play_space(pose))
		recenter_pending = false;
}

/* GL draws into the swapchain image (Linux) or our own framebuffer, the
 * image being acquired at release (Windows) */
GLuint Vr::acquire(Swapchain &sc)
{
#ifdef _WIN32
	return sc.render_fbo;
#else
	XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
	check(instance, xrAcquireSwapchainImage(sc.handle, &ai, &sc.index), "xrAcquireSwapchainImage");
	XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
	wi.timeout = XR_INFINITE_DURATION;
	check(instance, xrWaitSwapchainImage(sc.handle, &wi), "xrWaitSwapchainImage");
	return sc.fbos[sc.index];
#endif
}

void Vr::release(Swapchain &sc)
{
#ifdef _WIN32
	/* GL: flip into the shared texture (unlocking hands it to D3D) */
	wgl.LockObjects(interop_device, 1, &sc.interop);
	glDisable(GL_SCISSOR_TEST);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, sc.render_fbo);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sc.interop_fbo);
	glBlitFramebuffer(0, 0, sc.w, sc.h, 0, sc.h, sc.w, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	wgl.UnlockObjects(interop_device, 1, &sc.interop);
	XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
	if (!check(instance, xrAcquireSwapchainImage(sc.handle, &ai, &sc.index), "xrAcquireSwapchainImage"))
		return;
	XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
	wi.timeout = XR_INFINITE_DURATION;
	check(instance, xrWaitSwapchainImage(sc.handle, &wi), "xrWaitSwapchainImage");
	context->CopyResource(sc.images[sc.index], sc.shared);
#else
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
#endif
	XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
	xrReleaseSwapchainImage(sc.handle, &ri);
}

Vr::~Vr()
{
	if (session && running)
	{
		xrRequestExitSession(session);
		xrEndSession(session);
	}
	for (Swapchain *sc : {&eyes[0], &eyes[1], &screen})
	{
#ifdef _WIN32
		if (sc->interop)
			wgl.UnregisterObject(interop_device, sc->interop);
		com_release(sc->shared);
		glDeleteFramebuffers(1, &sc->render_fbo);
		glDeleteFramebuffers(1, &sc->interop_fbo);
		glDeleteTextures(1, &sc->render_tex);
		glDeleteTextures(1, &sc->interop_tex);
#else
		if (!sc->fbos.empty())
			glDeleteFramebuffers(GLsizei(sc->fbos.size()), sc->fbos.data());
#endif
		glDeleteRenderbuffers(1, &sc->stencil);
		if (sc->handle)
			xrDestroySwapchain(sc->handle);
	}
	for (XrSpace s : {play_space, base_space, view_space, aim_space[0], aim_space[1]})
		if (s)
			xrDestroySpace(s);
	if (action_set)
		xrDestroyActionSet(action_set);
	if (session)
		xrDestroySession(session);
	if (instance)
		xrDestroyInstance(instance);
#ifdef _WIN32
	if (interop_device)
		wgl.CloseDevice(interop_device);
	com_release(context);
	com_release(device);
#endif
}

bool Vr::init(float resolution_scale)
{
#ifdef _WIN32
	const char *exts[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
#elif defined(ANDROID)
	const char *exts[] = {XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME, XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME};
#else
	const char *exts[] = {XR_KHR_OPENGL_ENABLE_EXTENSION_NAME};
#endif
	XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
	std::strcpy(ici.applicationInfo.applicationName, "GLFrontier");
	ici.applicationInfo.applicationVersion = 1;
	std::strcpy(ici.applicationInfo.engineName, "GLFrontier");
	ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
	ici.enabledExtensionCount = uint32_t(std::size(exts));
	ici.enabledExtensionNames = exts;
#ifdef ANDROID
	/* The loader needs the VM and the activity before anything else */
	JNIEnv *env = static_cast<JNIEnv *>(SDL_AndroidGetJNIEnv());
	jobject local_activity = static_cast<jobject>(SDL_AndroidGetActivity());
	JavaVM *jvm = nullptr;
	if (!env || !local_activity || env->GetJavaVM(&jvm) != JNI_OK)
	{
		log_printf("openxr: no Java VM or activity\n");
		snprintf(status, sizeof status, "No Java VM or activity");
		return false;
	}
	jobject activity = env->NewGlobalRef(local_activity); /* (kept: the runtime may hold on to it) */
	PFN_xrInitializeLoaderKHR init_loader = nullptr;
	xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", reinterpret_cast<PFN_xrVoidFunction *>(&init_loader));
	if (!init_loader)
	{
		log_printf("openxr: the loader has no xrInitializeLoaderKHR\n");
		snprintf(status, sizeof status, "The OpenXR loader has no Android support");
		return false;
	}
	XrLoaderInitInfoAndroidKHR loader_info{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
	loader_info.applicationVM = jvm;
	loader_info.applicationContext = activity;
	if (!check(nullptr, init_loader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR *>(&loader_info)),
			   "xrInitializeLoaderKHR"))
		return false;
	XrInstanceCreateInfoAndroidKHR android_info{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
	android_info.applicationVM = jvm;
	android_info.applicationActivity = activity;
	ici.next = &android_info;
#endif
	if (!check(nullptr, xrCreateInstance(&ici, &instance), "xrCreateInstance (is an OpenXR runtime installed and active?)"))
		return false;
	XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
	sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	if (!check(instance, xrGetSystem(instance, &sgi, &system), "xrGetSystem (headset connected, VR runtime running?)"))
		return false;
	XrSystemProperties props{XR_TYPE_SYSTEM_PROPERTIES};
	xrGetSystemProperties(instance, system, &props);

#ifdef _WIN32
	if (!make_device())
		return false;
	XrGraphicsBindingD3D11KHR gb{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
	gb.device = device;
#elif defined(ANDROID)
	PFN_xrGetOpenGLESGraphicsRequirementsKHR get_reqs = nullptr;
	xrGetInstanceProcAddr(instance, "xrGetOpenGLESGraphicsRequirementsKHR",
						  reinterpret_cast<PFN_xrVoidFunction *>(&get_reqs));
	XrGraphicsRequirementsOpenGLESKHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
	if (!get_reqs || !check(instance, get_reqs(instance, system, &reqs), "xrGetOpenGLESGraphicsRequirementsKHR"))
		return false;
	log_printf("openxr: OpenGL ES %d.%d to %d.%d wanted\n", XR_VERSION_MAJOR(reqs.minApiVersionSupported),
			   XR_VERSION_MINOR(reqs.minApiVersionSupported), XR_VERSION_MAJOR(reqs.maxApiVersionSupported),
			   XR_VERSION_MINOR(reqs.maxApiVersionSupported));

	/* the runtime shares our EGL context */
	XrGraphicsBindingOpenGLESAndroidKHR gb{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
	gb.display = eglGetCurrentDisplay();
	gb.context = eglGetCurrentContext();
	if (gb.display == EGL_NO_DISPLAY || gb.context == EGL_NO_CONTEXT)
	{
		log_printf("openxr: no current EGL context\n");
		snprintf(status, sizeof status, "No current EGL context");
		return false;
	}
	EGLint config_id = 0, nconfigs = 0;
	eglQueryContext(gb.display, gb.context, EGL_CONFIG_ID, &config_id);
	const EGLint config_attrs[] = {EGL_CONFIG_ID, config_id, EGL_NONE};
	if (!eglChooseConfig(gb.display, config_attrs, &gb.config, 1, &nconfigs) || nconfigs < 1)
	{
		log_printf("openxr: cannot find the EGL config of the context\n");
		snprintf(status, sizeof status, "No EGL config for the context");
		return false;
	}
#else
	PFN_xrGetOpenGLGraphicsRequirementsKHR get_reqs = nullptr;
	xrGetInstanceProcAddr(instance, "xrGetOpenGLGraphicsRequirementsKHR",
						  reinterpret_cast<PFN_xrVoidFunction *>(&get_reqs));
	XrGraphicsRequirementsOpenGLKHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR};
	if (!get_reqs || !check(instance, get_reqs(instance, system, &reqs), "xrGetOpenGLGraphicsRequirementsKHR"))
		return false;

	/* the runtime shares our GLX context */
	XrGraphicsBindingOpenGLXlibKHR gb{XR_TYPE_GRAPHICS_BINDING_OPENGL_XLIB_KHR};
	gb.xDisplay = glXGetCurrentDisplay();
	gb.glxContext = glXGetCurrentContext();
	gb.glxDrawable = glXGetCurrentDrawable();
	if (!gb.xDisplay || !gb.glxContext)
	{
		log_printf("openxr: needs a GLX (X11) OpenGL context\n");
		snprintf(status, sizeof status, "Needs an X11 (GLX) OpenGL context");
		return false;
	}
	int fbconfig_id = 0, screen_no = DefaultScreen(gb.xDisplay);
	glXQueryContext(gb.xDisplay, gb.glxContext, GLX_FBCONFIG_ID, &fbconfig_id);
	glXQueryContext(gb.xDisplay, gb.glxContext, GLX_SCREEN, &screen_no);
	int attrs[] = {GLX_FBCONFIG_ID, fbconfig_id, None}, n = 0;
	if (GLXFBConfig *cfgs = glXChooseFBConfig(gb.xDisplay, screen_no, attrs, &n))
	{
		if (n > 0)
		{
			gb.glxFBConfig = cfgs[0];
			if (XVisualInfo *vi = glXGetVisualFromFBConfig(gb.xDisplay, cfgs[0]))
			{
				gb.visualid = uint32_t(vi->visualid);
				XFree(vi);
			}
		}
		XFree(cfgs);
	}
#endif
	XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
	sci.next = &gb;
	sci.systemId = system;
	if (!check(instance, xrCreateSession(instance, &sci, &session), "xrCreateSession"))
		return false;

	XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
	rs.poseInReferenceSpace.orientation.w = 1;
	rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	if (!check(instance, xrCreateReferenceSpace(session, &rs, &base_space), "xrCreateReferenceSpace"))
		return false;
	rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	if (!check(instance, xrCreateReferenceSpace(session, &rs, &view_space), "xrCreateReferenceSpace"))
		return false;
	if (!make_play_space(rs.poseInReferenceSpace))
		return false;
	if (!make_actions())
		log_printf("openxr: no motion controllers\n");

	uint32_t nv = 0;
	xrEnumerateViewConfigurationViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &nv, nullptr);
	std::vector<XrViewConfigurationView> vcv(nv, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
	xrEnumerateViewConfigurationViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, nv, &nv, vcv.data());
	if (nv != 2)
	{
		log_printf("openxr: expected 2 views, got %u\n", nv);
		snprintf(status, sizeof status, "Not a stereo headset");
		return false;
	}

	/* sRGB swapchain written without GL_FRAMEBUFFER_SRGB: the game's colours
	 * are already sRGB-encoded, and the runtime decodes them */
	uint32_t nf = 0;
	xrEnumerateSwapchainFormats(session, 0, &nf, nullptr);
	std::vector<int64_t> formats(nf);
	xrEnumerateSwapchainFormats(session, nf, &nf, formats.data());
	int64_t format = 0;
	for (int64_t want : {kFormatSrgb, kFormatRgba})
		if (!format && std::find(formats.begin(), formats.end(), want) != formats.end())
			format = want;
	if (!format)
	{
		log_printf("openxr: no usable swapchain format\n");
		snprintf(status, sizeof status, "No usable swapchain format");
		return false;
	}
	for (int i = 0; i < 2; i++)
	{
		int w = std::clamp(int(vcv[i].recommendedImageRectWidth * resolution_scale), 16, int(vcv[i].maxImageRectWidth));
		int h = std::clamp(int(vcv[i].recommendedImageRectHeight * resolution_scale), 16, int(vcv[i].maxImageRectHeight));
		if (!make_swapchain(eyes[i], format, w, h))
			return false;
	}
	if (!make_swapchain(screen, format, VR_SCREEN_W, VR_SCREEN_H))
		return false;
	log_printf("openxr: %s, %d x %d per eye (recommended %u x %u), %s swapchains\n", props.systemName, eyes[0].w,
			   eyes[0].h, vcv[0].recommendedImageRectWidth, vcv[0].recommendedImageRectHeight,
			   format == kFormatSrgb ? "sRGB" : "RGBA8");
	snprintf(status, sizeof status, "%s: waiting for the runtime", props.systemName);
	return true;
}

void Vr::poll()
{
	XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
	while (xrPollEvent(instance, &ev) == XR_SUCCESS)
	{
		if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
		{
			auto &e = *reinterpret_cast<XrEventDataSessionStateChanged *>(&ev);
			if (e.state == XR_SESSION_STATE_READY)
			{
				XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
				bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
				if (check(instance, xrBeginSession(session, &bi), "xrBeginSession"))
				{
					running = true;
					recenter_pending = true;
					log_printf("openxr: session running\n");
					snprintf(status, sizeof status, "Running, %d x %d per eye", eyes[0].w, eyes[0].h);
				}
			}
			else if (e.state == XR_SESSION_STATE_FOCUSED || e.state == XR_SESSION_STATE_VISIBLE)
			{
				focused = e.state == XR_SESSION_STATE_FOCUSED;
			}
			else if (e.state == XR_SESSION_STATE_STOPPING)
			{
				xrEndSession(session);
				running = focused = false;
				log_printf("openxr: session stopped\n");
				snprintf(status, sizeof status, "Paused by the runtime");
			}
			else if (e.state == XR_SESSION_STATE_EXITING || e.state == XR_SESSION_STATE_LOSS_PENDING)
			{
				running = false;
				lost = true;
			}
		}
		else if (ev.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED && action_set)
		{
			for (int i = 0; i < 2; i++)
			{
				XrInteractionProfileState st{XR_TYPE_INTERACTION_PROFILE_STATE};
				char name[XR_MAX_PATH_LENGTH] = "none";
				uint32_t len = 0;
				if (XR_SUCCEEDED(xrGetCurrentInteractionProfile(session, hand_paths[i], &st)) &&
					st.interactionProfile != XR_NULL_PATH)
					xrPathToString(instance, st.interactionProfile, sizeof name, &len, name);
				log_printf("openxr: %s controller: %s\n", i ? "right" : "left", name);
			}
		}
		else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING)
		{
			running = false;
			lost = true;
		}
		ev = {XR_TYPE_EVENT_DATA_BUFFER};
	}
}

bool Vr::begin_frame()
{
	frame = {XR_TYPE_FRAME_STATE};
	XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
	if (!check(instance, xrWaitFrame(session, &wi, &frame), "xrWaitFrame"))
		return false;
	XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
	if (!check(instance, xrBeginFrame(session, &bi), "xrBeginFrame"))
		return false;
	in_frame = true;
	drew_eyes = drew_screen = drew_overlay = false;
	if (recenter_pending)
		try_recenter(frame.predictedDisplayTime);
	read_hands();

	XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
	li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	li.displayTime = frame.predictedDisplayTime;
	li.space = play_space;
	XrViewState vs{XR_TYPE_VIEW_STATE};
	uint32_t n = 0;
	views_valid = XR_SUCCEEDED(xrLocateViews(session, &li, &vs, 2, &n, views)) && n == 2 &&
				  (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
	/* A failing positional tracker can report the head kilometres away. In
	 * a seat that is never real: drop the head position but keep the eyes'
	 * separation. The submitted poses are changed too, so the compositor
	 * reprojects what was drawn. */
	if (views_valid)
	{
		XrVector3f c{};
		for (const XrView &v : views)
		{
			c.x += v.pose.position.x / 2;
			c.y += v.pose.position.y / 2;
			c.z += v.pose.position.z / 2;
		}
		float dist = std::sqrt(c.x * c.x + c.y * c.y + c.z * c.z);
		bool bad = !(dist <= kMaxHeadOffsetM); /* also NaN */
		if (bad)
			for (XrView &v : views)
			{
				v.pose.position.x -= c.x;
				v.pose.position.y -= c.y;
				v.pose.position.z -= c.z;
				if (!std::isfinite(v.pose.position.x + v.pose.position.y + v.pose.position.z))
					v.pose.position = {};
			}
		if (bad != position_rejected)
		{
			if (bad)
				log_printf("openxr: head position %.1f m from the seat; ignoring position (tracking lost?)\n", dist);
			else
				log_printf("openxr: head position back near the seat\n");
			position_rejected = bad;
		}
	}
	frames_since_log++;
	if (log && frame.predictedDisplayTime - last_log >= 500000000)
	{
		double fps = last_log ? frames_since_log * 1e9 / double(frame.predictedDisplayTime - last_log) : 0;
		last_log = frame.predictedDisplayTime;
		frames_since_log = 0;
		const XrPosef &p = views[0].pose;
		log_printf("openxr: %.1f fps, left eye pos %+.3f %+.3f %+.3f m, quat %+.3f %+.3f %+.3f %+.3f, flags %llx\n", fps,
				   p.position.x, p.position.y, p.position.z, p.orientation.x, p.orientation.y, p.orientation.z,
				   p.orientation.w, (unsigned long long)vs.viewStateFlags);
		for (int i = 0; i < 2; i++)
		{
			const VrHand &h = hands[i];
			if (h.connected)
				log_printf("openxr: %s hand at %+.3f %+.3f %+.3f m, pointing %+.3f %+.3f %+.3f, trigger %.2f, stick "
						   "%+.2f %+.2f, pad %+.2f %+.2f,%s%s%s%s%s%s\n",
						   i ? "right" : "left", h.pos[0], h.pos[1], h.pos[2], h.dir[0], h.dir[1], h.dir[2], h.trigger,
						   h.stick[0], h.stick[1], h.pad[0], h.pad[1], h.squeeze ? " grip" : "",
						   h.stick_click ? " stick-click" : "", h.pad_click ? " pad-click" : "", h.menu ? " menu" : "",
						   h.lower ? " lower" : "", h.upper ? " upper" : "");
		}
	}
	return frame.shouldRender;
}

void Vr::end_frame()
{
	if (!in_frame)
		return;
	in_frame = false;
	XrCompositionLayerProjectionView pv[2];
	XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
	XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
	const XrCompositionLayerBaseHeader *layers[2];
	uint32_t count = 0;
	if (drew_eyes)
	{
		for (int i = 0; i < 2; i++)
		{
			pv[i] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
			pv[i].pose = views[i].pose;
			pv[i].fov = views[i].fov;
			pv[i].subImage.swapchain = eyes[i].handle;
			pv[i].subImage.imageRect = {{0, 0}, {eyes[i].w, eyes[i].h}};
		}
		proj.space = play_space;
		proj.viewCount = 2;
		proj.views = pv;
		layers[count++] = reinterpret_cast<const XrCompositionLayerBaseHeader *>(&proj);
	}
	else if (drew_screen)
	{
		quad.space = play_space;
		quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		quad.subImage.swapchain = screen.handle;
		quad.subImage.imageRect = {{0, 0}, {screen.w, screen.h}};
		quad.pose.orientation.w = 1;
		float dist, width, drop;
		vr_screen_place(&dist, &width, &drop);
		quad.pose.position = {0, -drop, -dist};
		quad.size = {width, width * 0.75f};
		layers[count++] = reinterpret_cast<const XrCompositionLayerBaseHeader *>(&quad);
		if (drew_overlay)
		{
			for (int i = 0; i < 2; i++)
			{
				pv[i] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
				pv[i].pose = views[i].pose;
				pv[i].fov = views[i].fov;
				pv[i].subImage.swapchain = eyes[i].handle;
				pv[i].subImage.imageRect = {{0, 0}, {eyes[i].w, eyes[i].h}};
			}
			proj.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
			proj.space = play_space;
			proj.viewCount = 2;
			proj.views = pv;
			layers[count++] = reinterpret_cast<const XrCompositionLayerBaseHeader *>(&proj);
		}
	}
	XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
	ei.displayTime = frame.predictedDisplayTime;
	ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	ei.layerCount = count;
	ei.layers = layers;
	check(instance, xrEndFrame(session, &ei), "xrEndFrame");
}

void to_view(const XrView &v, VrView *out)
{
	const XrQuaternionf &q = v.pose.orientation;
	const float R[3][3] = {
		{1 - 2 * (q.y * q.y + q.z * q.z), 2 * (q.x * q.y - q.z * q.w), 2 * (q.x * q.z + q.y * q.w)},
		{2 * (q.x * q.y + q.z * q.w), 1 - 2 * (q.x * q.x + q.z * q.z), 2 * (q.y * q.z - q.x * q.w)},
		{2 * (q.x * q.z - q.y * q.w), 2 * (q.y * q.z + q.x * q.w), 1 - 2 * (q.x * q.x + q.y * q.y)},
	};
	std::memcpy(out->rot, R, sizeof R);
	out->pos[0] = v.pose.position.x;
	out->pos[1] = v.pose.position.y;
	out->pos[2] = v.pose.position.z;
	out->tan_left = std::tan(v.fov.angleLeft);
	out->tan_right = std::tan(v.fov.angleRight);
	out->tan_down = std::tan(v.fov.angleDown);
	out->tan_up = std::tan(v.fov.angleUp);
}

} // namespace

extern "C"
{

void vr_update(void)
{
	static bool was_enabled;
	if (vr_enabled != was_enabled)
		vr_failed = false; /* toggled: try again */
	was_enabled = vr_enabled;

	if (vr && (!vr_enabled || vr->lost))
	{
		if (vr->lost)
		{
			log_printf("openxr: the runtime ended the session\n");
			vr_enabled = was_enabled = false;
		}
		vr_shutdown();
		snprintf(status, sizeof status, "Off");
	}
	if (vr_enabled && !vr && !vr_failed)
	{
		vr = new Vr;
		if (!vr->init(std::clamp(vr_resolution, 25, 200) / 100.0f))
		{
			delete vr;
			vr = nullptr;
			vr_failed = true;
			glBindFramebuffer(GL_FRAMEBUFFER, 0);
			log_printf("openxr: VR not started\n");
		}
	}
	if (vr)
		vr->poll();
}

void vr_shutdown(void)
{
	delete vr;
	vr = nullptr;
}

bool vr_running(void)
{
	return vr && vr->running;
}

const char *vr_status(void)
{
	return status;
}

bool vr_begin_frame(void)
{
	return vr && vr->begin_frame();
}

bool vr_tracking(void)
{
	return vr && vr->in_frame && vr->views_valid;
}

void vr_view(int eye, VrView *out)
{
	to_view(vr->views[eye & 1], out);
}

void vr_centre_view(VrView *out)
{
	XrView v = vr->views[0];
	const XrView &r = vr->views[1];
	v.pose.position.x = (v.pose.position.x + r.pose.position.x) / 2;
	v.pose.position.y = (v.pose.position.y + r.pose.position.y) / 2;
	v.pose.position.z = (v.pose.position.z + r.pose.position.z) / 2;
	/* the eyes' orientations, averaged (the same unless the displays are canted) */
	XrQuaternionf &a = v.pose.orientation;
	const XrQuaternionf &b = r.pose.orientation;
	float s = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0 ? -1.0f : 1.0f;
	a = {a.x + s * b.x, a.y + s * b.y, a.z + s * b.z, a.w + s * b.w};
	float len = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z + a.w * a.w);
	if (len > 1e-6f)
		a = {a.x / len, a.y / len, a.z / len, a.w / len};
	else
		a = r.pose.orientation;
	v.fov.angleLeft = std::min(v.fov.angleLeft, r.fov.angleLeft);
	v.fov.angleRight = std::max(v.fov.angleRight, r.fov.angleRight);
	v.fov.angleDown = std::min(v.fov.angleDown, r.fov.angleDown);
	v.fov.angleUp = std::max(v.fov.angleUp, r.fov.angleUp);
	to_view(v, out);
}

void vr_eye_size(int eye, int *w, int *h)
{
	*w = vr->eyes[eye & 1].w;
	*h = vr->eyes[eye & 1].h;
}

unsigned vr_acquire_eye(int eye)
{
	vr->drew_eyes = true;
	return vr->acquire(vr->eyes[eye & 1]);
}

void vr_release_eye(int eye)
{
	vr->release(vr->eyes[eye & 1]);
}

unsigned vr_acquire_screen(void)
{
	vr->drew_screen = true;
	return vr->acquire(vr->screen);
}

void vr_release_screen(void)
{
	vr->release(vr->screen);
}

bool vr_controller(int hand, VrHand *out)
{
	*out = vr && vr->in_frame ? vr->hands[hand & 1] : VrHand{};
	return out->connected;
}

void vr_buzz(int hand, float seconds, float amplitude)
{
	if (!vr || !vr->haptic || !vr->focused)
		return;
	XrHapticActionInfo hi{XR_TYPE_HAPTIC_ACTION_INFO};
	hi.action = vr->haptic;
	hi.subactionPath = vr->hand_paths[hand & 1];
	XrHapticVibration v{XR_TYPE_HAPTIC_VIBRATION};
	v.duration = XrDuration(seconds * 1e9f);
	v.frequency = XR_FREQUENCY_UNSPECIFIED;
	v.amplitude = amplitude;
	xrApplyHapticFeedback(vr->session, &hi, reinterpret_cast<const XrHapticBaseHeader *>(&v));
}

unsigned vr_acquire_overlay_eye(int eye)
{
	vr->drew_overlay = true;
	return vr->acquire(vr->eyes[eye & 1]);
}

void vr_release_overlay_eye(int eye)
{
	vr->release(vr->eyes[eye & 1]);
}

void vr_end_frame(void)
{
	if (vr)
		vr->end_frame();
}

void vr_recenter(void)
{
	if (vr)
		vr->recenter_pending = true;
}

} /* extern "C" */

#else /* no VR in this build */

bool vr_enabled = false;
int vr_resolution = 100;

extern "C"
{
void vr_update(void) {}
void vr_shutdown(void) {}
bool vr_running(void) { return false; }
const char *vr_status(void) { return "Not in this build"; }
bool vr_begin_frame(void) { return false; }
bool vr_tracking(void) { return false; }
void vr_view(int, VrView *) {}
void vr_centre_view(VrView *) {}
void vr_eye_size(int, int *w, int *h) { *w = *h = 0; }
unsigned vr_acquire_eye(int) { return 0; }
void vr_release_eye(int) {}
unsigned vr_acquire_screen(void) { return 0; }
void vr_release_screen(void) {}
void vr_end_frame(void) {}
bool vr_controller(int, VrHand *out)
{
	*out = VrHand{};
	return false;
}
void vr_buzz(int, float, float) {}
unsigned vr_acquire_overlay_eye(int) { return 0; }
void vr_release_overlay_eye(int) {}
void vr_recenter(void) {}
}

#endif /* GLF_VR */
