/* Regenerated TG5040 pvrfb port from pinned desktop source and recovered
 * successful port patches. Not claimed identical to the lost final source.
 * Preserve the tested emulator logical mapping; launcher A/B is separate. */
/* <SDL.h>, not <SDL2/SDL.h>: sdl2-config and pkg-config both put the SDL2
 * directory on the include path, and this is the form SDL documents. */
#include <SDL.h>
#include <stdio.h>
#include <pthread.h>
#include <string.h>
#include <alsa/asoundlib.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "diatom_port.h"
#include "port_clock.h"
#include "ladder.h"

#define WINDOW_W 1280
#define WINDOW_H 720
#define AUDIO_RATE 48000
/* Capacity in FRAMES (one frame = two int16 samples). 4096 at 48kHz is ~85ms,
 * with rate control aiming to hold it near half that. */
#define AUDIO_BUFFER_FRAMES 4096
#define AUDIO_FRAME_BYTES   (2 * (int)sizeof(int16_t))

static SDL_Window   *g_window;
static SDL_Renderer *g_renderer;
static SDL_Texture  *g_texture;
static int           g_tex_w, g_tex_h;
static Uint32        g_tex_fmt;
static diatom_filter g_tex_filter;
static SDL_AudioDeviceID g_audio;
static bool          g_quit;
static uint32_t      g_buttons;
static SDL_Joystick *g_pad;
static int g_keys[32], g_nkeys;
static int g_disp = -1;
static snd_mixer_t *g_mixer;
static bool g_muted, g_presented, g_menu_down;
static uint32_t g_key_buttons, g_level_down, g_level_pressed;
static uint32_t g_volume_as[2];
static int g_volume = -1, g_fn = -1;
static const unsigned char bright_ladder[] = {
	4, 6, 10, 16, 24, 32, 48, 64, 96, 128, 192, 255
};
static uint64_t g_osd_until;
static int g_osd_pos, g_osd_count;
static diatom_level_kind g_osd_kind;
static bool g_low, g_blink;
static void input_open(void);
static void settings_open(void);
static void settings_audio(void);
static void draw_indicators(void);

/* ---------- the audio sink ------------------------------------------------ */

/* Empty means the default device. */
static char g_audio_dev[128];

/* Closing an SDL audio device JOINS its audio thread, and that thread can be
 * blocked writing to a sink that has stopped draining. So this call can fail to
 * return, and on 2026-09-05 a stalled A2DP sink did exactly that.
 *
 * The obvious fix - close on a detached thread - was tried and REVERTED the
 * same day. SDL is not safe against opening a device while a close of another
 * is in flight: test/port_test.c fails three ways with it, the first being that
 * the fallback open after a refused device stops working. A frontend that
 * cannot reopen its audio is worse than one that can hang on a dead headset.
 *
 * The hang is also not really here. The frame loop calls SDL_QueueAudio and
 * SDL_GetQueuedAudioSize every frame, and those take the same device lock the
 * audio thread holds while it writes - so a stalled sink stops the loop whether
 * or not anything closes. Fixing that means not letting the write stall, which
 * is the sink question, not the teardown question.
 */
static bool audio_open(const char *name)
{
	SDL_AudioSpec want, have;

	if (g_audio) { SDL_CloseAudioDevice(g_audio); g_audio = 0; }

	SDL_memset(&want, 0, sizeof want);
	want.freq     = AUDIO_RATE;
	want.format   = AUDIO_S16SYS;
	want.channels = 2;
	want.samples  = 1024;

	if (SDL_WasInit(SDL_INIT_AUDIO) == 0 && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
		return false;
	settings_audio();
	/* allowed_changes 0: SDL converts behind this spec, so caps.audio_rate
	 * stays true across a reopen. */
	g_audio = SDL_OpenAudioDevice(name && *name ? name : NULL, 0,
	                              &want, &have, 0);
	if (!g_audio) return false;
	SDL_PauseAudioDevice(g_audio, 0);
	/* What SDL actually negotiated, not what was asked for. A device can open
	 * cleanly and then carry no sound - that is exactly what a bluealsa sink
	 * did on 2026-09-05 - and without this there is no way to tell from
	 * outside whether SDL agreed to something the device cannot service. */
	{
		char msg[192];

		snprintf(msg, sizeof msg,
		         "audio: opened %s at %d Hz, %d ch, %d frames/period, %u byte buffer",
		         name && *name ? name : "the default device",
		         have.freq, have.channels, have.samples, have.size);
		diatom_port_log(DIATOM_LOG_INFO, msg);
	}
	return true;
}

bool diatom_port_audio_set(const char *name, char *actual, size_t cap)
{
	const char *want = name ? name : "";
	bool ok;

	/* Already there. A write on the state plane is idempotent, so re-stating
	 * the current device must not close and reopen a working sink: that drops
	 * whatever is queued and costs a fresh A2DP stream setup. Repeated writes
	 * are not hypothetical - a launcher that recomputes its routing on a timer
	 * sends one whenever it thinks the answer might have moved. */
	if (g_audio && !strcmp(want, g_audio_dev)) {
		diatom_port_audio_get(actual, cap);
		return true;
	}

	ok = audio_open(name);

	if (!ok) {
		char msg[192];

		snprintf(msg, sizeof msg, "audio: %s unavailable (%s); using the default",
		         name && *name ? name : "the default device", SDL_GetError());
		diatom_port_log(DIATOM_LOG_WARN, msg);
		if (audio_open(NULL)) g_audio_dev[0] = '\0';
	} else {
		snprintf(g_audio_dev, sizeof g_audio_dev, "%s", name ? name : "");
	}
	diatom_port_audio_get(actual, cap);
	return ok;
}

/* Not at init: see diatom_port_audio_start in diatom_port.h. NOT fatal when
 * it fails. A port that cannot open a sound device can still put pixels where
 * the device wants them, and silence costs far less than no emulator: the
 * write path drops, and the host configures the session with capacity 0, which
 * turns dynamic rate control off rather than leaving it correcting for a
 * buffer that does not exist. Tried again at the next start, which costs
 * nothing when it fails. */
bool diatom_port_audio_start(void)
{
	if (g_audio) return true;
	if (audio_open(NULL)) {
		g_audio_dev[0] = '\0';
		return true;
	}
	fprintf(stderr, "audio unavailable (%s); continuing without sound\n", SDL_GetError());
	return false;
}

void diatom_port_audio_get(char *out, size_t cap)
{
	if (out && cap) snprintf(out, cap, "%s", g_audio_dev);
}

bool diatom_port_init(diatom_port_caps *out)
{

	/* Audio is initialised SEPARATELY and is allowed to fail. A subsystem that
	 * will not come up must not take the rest of the port with it - see the
	 * open below for why silence beats refusing to start. Rolled into one
	 * SDL_Init, a missing or busy audio device is indistinguishable from a
	 * missing display and kills both. */
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_JOYSTICK) != 0) {
		fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
		return false;
	}

	g_window = SDL_CreateWindow("diatom",
		SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
		WINDOW_W, WINDOW_H, SDL_WINDOW_SHOWN);
	if (!g_window) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return false; }

	g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED);
	if (!g_renderer) { fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError()); return false; }
	SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);

	/* Vsync OFF, deliberately.
	 *
	 * No console runs at the panel's rate - measured 50.0070, 59.7275, 59.8200,
	 * 60.0000 across six cores. Blocking on a 60Hz vblank while trying to hold
	 * 59.7275 leaves 0.07ms of slack per frame, so any jitter misses a vblank
	 * and costs a whole 16.67ms. That measured as a consistent 1.4% deficit.
	 *
	 * The host paces against a monotonic clock instead, and audio drift is
	 * absorbed by rate control rather than by hoping the panel agrees. */
	if (SDL_RenderSetVSync(g_renderer, 0) != 0)
		fprintf(stderr, "note: could not disable vsync: %s\n", SDL_GetError());
	SDL_ShowCursor(SDL_DISABLE);
	SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
	input_open();
	settings_open();

	/* NOT fatal. A port that cannot open a sound device can still put pixels
	 * where the device wants them, and refusing to start costs far more than
	 * silence does: Diatom is resident, so a failed open here leaves the
	 * launcher with no emulator at all and every launch then pays the cold
	 * cost (~1100 ms) instead of the warm one (~15 ms). That is the worst
	 * outcome available and it happened by accident - and a sink chosen at
	 * runtime needs this to be true, since anything settable while a game
	 * runs can fail while a game runs.
	 *
	 * capacity 0 is how the silence is reported rather than hidden: it turns
	 * off dynamic rate control, which would otherwise read a permanently empty
	 * queue as a permanent deficit and hold the resampler at its deviation
	 * limit forever, correcting for a buffer that does not exist. */
	/* No audio here: it opens once the launcher has drawn, or at the first
	 * game; see diatom_port_audio_start. */

	out->surface_w          = WINDOW_W;
	out->surface_h          = WINDOW_H;
	/* `have` is untouched when the open failed, so the rate comes from what
	 * was asked for: the resampler still needs a target to convert into,
	 * and a zero here would divide. */
	out->audio_rate         = AUDIO_RATE;
	out->audio_buffer_frames = AUDIO_BUFFER_FRAMES;   /* open or not yet */
	out->present_blocks     = false;
	return true;
}

void diatom_port_shutdown(void)
{
	if (g_pad) SDL_JoystickClose(g_pad);
	for (int i = 0; i < g_nkeys; i++) close(g_keys[i]);
	if (g_disp >= 0) close(g_disp);
	if (g_mixer) snd_mixer_close(g_mixer);
	if (g_audio)    SDL_CloseAudioDevice(g_audio);
	if (g_texture)  SDL_DestroyTexture(g_texture);
	if (g_renderer) SDL_DestroyRenderer(g_renderer);
	if (g_window)   SDL_DestroyWindow(g_window);
	SDL_Quit();
}

static bool ensure_texture(int w, int h, diatom_pixfmt fmt, diatom_filter filter)
{
	Uint32 sdlfmt = (fmt == DIATOM_PIX_RGB565)
	              ? SDL_PIXELFORMAT_RGB565
	              : SDL_PIXELFORMAT_ARGB8888;
	/* SDL has no sharp-bilinear, so this is an APPROXIMATION: linear blends
	 * across the whole source pixel where sharp-bilinear blends across one
	 * destination pixel. Exact sharp-bilinear remains a panel-quality gate. */
	SDL_ScaleMode mode = (filter == DIATOM_FILTER_SHARP)
	                   ? SDL_ScaleModeLinear
	                   : SDL_ScaleModeNearest;

	if (g_texture && g_tex_w == w && g_tex_h == h && g_tex_fmt == sdlfmt) {
		if (filter != g_tex_filter) {
			SDL_SetTextureScaleMode(g_texture, mode);
			g_tex_filter = filter;
		}
		return true;
	}

	if (g_texture) SDL_DestroyTexture(g_texture);
	g_texture = SDL_CreateTexture(g_renderer, sdlfmt,
	                              SDL_TEXTUREACCESS_STREAMING, w, h);
	if (!g_texture) {
		fprintf(stderr, "SDL_CreateTexture: %s\n", SDL_GetError());
		return false;
	}
	SDL_SetTextureScaleMode(g_texture, mode);
	g_tex_w = w; g_tex_h = h; g_tex_fmt = sdlfmt; g_tex_filter = filter;
	return true;
}

/* The overlay from diatom_port_overlay: a borrowed pointer, not a copy. The
 * texture is rebuilt only when the image changes, which for a notice on a
 * timer is once. */
static const uint8_t *g_ov;
static int            g_ov_w, g_ov_h;
static uint64_t       g_ov_until;
static SDL_Texture   *g_ov_tex;

void diatom_port_overlay(const uint8_t *bgra, int w, int h, unsigned ms)
{
	int sw = 0, sh = 0;

	if (g_ov_tex) { SDL_DestroyTexture(g_ov_tex); g_ov_tex = NULL; }
	g_ov = NULL;
	g_ov_until = 0;
	if (!bgra || ms == 0 || w <= 0 || h <= 0) return;

	SDL_GetRendererOutputSize(g_renderer, &sw, &sh);
	/* Refused, not clipped - see diatom_port.h. */
	if ((sw && w > sw) || (sh && h > sh)) {
		diatom_port_log(DIATOM_LOG_WARN, "overlay larger than the window; ignored");
		return;
	}

	g_ov_tex = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ARGB8888,
	                             SDL_TEXTUREACCESS_STATIC, w, h);
	if (!g_ov_tex) return;
	SDL_SetTextureBlendMode(g_ov_tex, SDL_BLENDMODE_BLEND);
	SDL_UpdateTexture(g_ov_tex, NULL, bgra, w * 4);
	g_ov = bgra;
	g_ov_w = w;
	g_ov_h = h;
	g_ov_until = diatom_port_now_us() + (uint64_t)ms * 1000ull;
}

static void draw_overlay(void)
{
	SDL_Rect r;
	int sw = 0, sh = 0;

	if (!g_ov_tex || diatom_port_now_us() >= g_ov_until) return;
	SDL_GetRendererOutputSize(g_renderer, &sw, &sh);
	r.w = g_ov_w;
	r.h = g_ov_h;
	r.x = (sw - g_ov_w) / 2;
	r.y = sh - g_ov_h - sh / 24;
	SDL_RenderCopy(g_renderer, g_ov_tex, NULL, &r);
}

void diatom_port_present(const void *src, int w, int h, size_t pitch,
                         diatom_pixfmt fmt, diatom_rect dst,
                         diatom_filter filter)
{
	SDL_Rect r;

	if (w > 0 && h > 0 && !ensure_texture(w, h, fmt, filter)) return;
	if (src && g_texture) SDL_UpdateTexture(g_texture, NULL, src, (int)pitch);

	SDL_RenderClear(g_renderer);
	if (g_texture) {
		r.x = dst.x; r.y = dst.y; r.w = dst.w; r.h = dst.h;
		SDL_RenderCopy(g_renderer, g_texture, NULL, &r);
	}
	draw_overlay();
	draw_indicators();
	SDL_RenderPresent(g_renderer);
	g_presented = true;
}

size_t diatom_port_audio_write(const int16_t *frames, size_t n)
{
	size_t queued, room;

	/* Never blocks; drops on overflow. A blocking write would pace the whole
	 * program off the audio clock, which rules out dynamic rate control.
	 *
	 * Take only what fits. The previous test was `queued >= capacity`, which
	 * refuses only an ALREADY FULL queue and then admits a whole batch: at 4095
	 * queued a 832-frame batch landed entire, and the queue reached a measured
	 * **4927 against a stated 4096**. That mattered beyond the extra latency,
	 * because rate control clamps its error term at +1.0 - reached at exactly
	 * capacity - so everything above capacity was invisible to the controller
	 * that was supposed to be preventing it.
	 *
	 * Clamping rather than refusing the batch outright loses less: the frames
	 * that do not fit are dropped either way, and there is no reason to discard
	 * the ones that would have. */
	/* A sink can die under the port - a headset switched off, or carried out
	 * of range. SDL reports the device STOPPED; falling back happens here
	 * rather than in a poll of its own because this already runs every frame,
	 * and the host finds out through diatom_port_audio_get, which is the only
	 * way it can (the port cannot announce anything).
	 *
	 * Only when a name was asked for. If the DEFAULT device dies there is
	 * nowhere better to go, and retrying it every frame would be a reopen
	 * storm in place of silence. */
	if (g_audio && g_audio_dev[0] &&
	    SDL_GetAudioDeviceStatus(g_audio) == SDL_AUDIO_STOPPED) {
		diatom_port_log(DIATOM_LOG_WARN,
		                "audio: the sink went away; falling back to the default");
		g_audio_dev[0] = '\0';
		audio_open(NULL);
	}
	if (!g_audio || !frames || !n) return 0;
	queued = diatom_port_audio_queued();
	room   = queued >= (size_t)AUDIO_BUFFER_FRAMES
	       ? 0 : (size_t)AUDIO_BUFFER_FRAMES - queued;
	if (n > room) n = room;
	if (!n) return 0;

	SDL_QueueAudio(g_audio, frames, (Uint32)(n * AUDIO_FRAME_BYTES));
	return n;
}

size_t diatom_port_audio_queued(void)
{
	if (!g_audio) return 0;
	return SDL_GetQueuedAudioSize(g_audio) / AUDIO_FRAME_BYTES;
}

static const struct { SDL_Scancode key; int btn; } keymap[] = {
	{ SDL_SCANCODE_UP,     DIATOM_BTN_UP     },
	{ SDL_SCANCODE_DOWN,   DIATOM_BTN_DOWN   },
	{ SDL_SCANCODE_LEFT,   DIATOM_BTN_LEFT   },
	{ SDL_SCANCODE_RIGHT,  DIATOM_BTN_RIGHT  },
	{ SDL_SCANCODE_X,      DIATOM_BTN_A      },
	{ SDL_SCANCODE_Z,      DIATOM_BTN_B      },
	{ SDL_SCANCODE_S,      DIATOM_BTN_X      },
	{ SDL_SCANCODE_A,      DIATOM_BTN_Y      },
	{ SDL_SCANCODE_Q,      DIATOM_BTN_L1     },
	{ SDL_SCANCODE_W,      DIATOM_BTN_R1     },
	{ SDL_SCANCODE_1,      DIATOM_BTN_L2     },
	{ SDL_SCANCODE_2,      DIATOM_BTN_R2     },
	{ SDL_SCANCODE_RSHIFT, DIATOM_BTN_SELECT },
	{ SDL_SCANCODE_RETURN, DIATOM_BTN_START  },
};

static const int padmap[] = {
	DIATOM_BTN_B, DIATOM_BTN_A, DIATOM_BTN_Y, DIATOM_BTN_X,
	DIATOM_BTN_L1, DIATOM_BTN_R1, DIATOM_BTN_SELECT, DIATOM_BTN_START
};

static void menu_event(bool down, bool *pulse)
{
	g_menu_down = down;
	if (down) *pulse = true;
}

static void volume_event(int k, bool down)
{
	if (down && !g_volume_as[k]) {
		g_volume_as[k] = g_menu_down
			? (k ? DIATOM_LEVEL_KEY_BRIGHTNESS_DOWN : DIATOM_LEVEL_KEY_BRIGHTNESS_UP)
			: (k ? DIATOM_LEVEL_KEY_VOLUME_DOWN : DIATOM_LEVEL_KEY_VOLUME_UP);
		g_level_down |= g_volume_as[k];
		g_level_pressed |= g_volume_as[k];
	} else if (!down) {
		g_level_down &= ~g_volume_as[k];
		g_volume_as[k] = 0;
	}
}

static void input_open(void)
{
	if (SDL_NumJoysticks() > 0) g_pad = SDL_JoystickOpen(0);
	for (int i = 0; i < 32; i++) {
		char path[64];
		unsigned char bits[KEY_MAX / 8 + 1] = { 0 };
		snprintf(path, sizeof path, "/dev/input/event%d", i);
		int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0) continue;
		ioctl(fd, EVIOCGBIT(EV_KEY, sizeof bits), bits);
		if ((bits[KEY_VOLUMEUP / 8] & (1 << (KEY_VOLUMEUP % 8))) ||
		    (bits[KEY_VOLUMEDOWN / 8] & (1 << (KEY_VOLUMEDOWN % 8))))
			g_keys[g_nkeys++] = fd;
		else close(fd);
	}
}

void diatom_port_input_poll(void)
{
	SDL_Event ev;
	uint32_t pulse = 0, s = 0;
	bool menu = false;
	FILE *f = fopen("/sys/class/gpio/gpio243/value", "r");
	int value;
	if (f) {
		if (fscanf(f, "%d", &value) == 1 && (value != 0) != g_fn) {
			g_fn = value != 0;
			diatom_port_mute_set(g_fn);
		}
		fclose(f);
	}
	while (SDL_PollEvent(&ev)) {
		if (ev.type == SDL_QUIT) g_quit = true;
		if ((ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) && !ev.key.repeat) {
			bool down = ev.type == SDL_KEYDOWN;
			for (size_t i = 0; i < sizeof keymap / sizeof keymap[0]; i++) {
				if (ev.key.keysym.scancode != keymap[i].key) continue;
				uint32_t bit = DIATOM_BIT(keymap[i].btn);
				if (down) { g_key_buttons |= bit; pulse |= bit; }
				else g_key_buttons &= ~bit;
			}
			if (ev.key.keysym.sym == SDLK_ESCAPE || ev.key.keysym.sym == SDLK_TAB)
				menu_event(down, &menu);
			if (ev.key.keysym.sym == SDLK_EQUALS) volume_event(0, down);
			if (ev.key.keysym.sym == SDLK_MINUS) volume_event(1, down);
		}
		if (g_pad && (ev.type == SDL_JOYBUTTONDOWN || ev.type == SDL_JOYBUTTONUP) &&
		    ev.jbutton.which == SDL_JoystickInstanceID(g_pad)) {
			bool down = ev.type == SDL_JOYBUTTONDOWN;
			if (ev.jbutton.button == 8) menu_event(down, &menu);
			if (ev.jbutton.button == 128) volume_event(0, down);
			if (ev.jbutton.button == 129) volume_event(1, down);
			if (down && ev.jbutton.button < sizeof padmap / sizeof padmap[0])
				pulse |= DIATOM_BIT(padmap[ev.jbutton.button]);
		}
	}
	struct input_event key;
	for (int i = 0; i < g_nkeys; i++)
		while (read(g_keys[i], &key, sizeof key) == sizeof key) {
			if (key.type != EV_KEY || key.value == 2) continue;
			if (key.code == KEY_VOLUMEUP) volume_event(0, key.value == 1);
			if (key.code == KEY_VOLUMEDOWN) volume_event(1, key.value == 1);
		}
	if (g_pad && SDL_JoystickGetAttached(g_pad)) {
		for (size_t i = 0; i < sizeof padmap / sizeof padmap[0]; i++)
			if (SDL_JoystickGetButton(g_pad, i)) s |= DIATOM_BIT(padmap[i]);
		if (SDL_JoystickGetAxis(g_pad, 2) > 0) s |= DIATOM_BIT(DIATOM_BTN_L2);
		if (SDL_JoystickGetAxis(g_pad, 5) > 0) s |= DIATOM_BIT(DIATOM_BTN_R2);
		Uint8 hat = SDL_JoystickGetHat(g_pad, 0);
		if (hat & SDL_HAT_LEFT) s |= DIATOM_BIT(DIATOM_BTN_LEFT);
		if (hat & SDL_HAT_RIGHT) s |= DIATOM_BIT(DIATOM_BTN_RIGHT);
		if (hat & SDL_HAT_UP) s |= DIATOM_BIT(DIATOM_BTN_UP);
		if (hat & SDL_HAT_DOWN) s |= DIATOM_BIT(DIATOM_BTN_DOWN);
		for (int i = 0; i < 2; i++) {
			int x = SDL_JoystickGetAxis(g_pad, i ? 3 : 0);
			int y = SDL_JoystickGetAxis(g_pad, i ? 4 : 1);
			if (x < -12000) s |= DIATOM_BIT(DIATOM_BTN_LEFT);
			if (x > 12000) s |= DIATOM_BIT(DIATOM_BTN_RIGHT);
			if (y < -12000) s |= DIATOM_BIT(DIATOM_BTN_UP);
			if (y > 12000) s |= DIATOM_BIT(DIATOM_BTN_DOWN);
		}
	}
	g_buttons = s | g_key_buttons | pulse |
		((g_menu_down || menu) ? DIATOM_BIT(DIATOM_BTN_MENU) : 0);
}

uint32_t diatom_port_input_state(void) { return g_buttons; }

void diatom_port_input_reset(void)
{
	SDL_Event ev;
	struct input_event key;
	SDL_PumpEvents();
	while (SDL_PollEvent(&ev)) if (ev.type == SDL_QUIT) g_quit = true;
	for (int i = 0; i < g_nkeys; i++)
		while (read(g_keys[i], &key, sizeof key) == sizeof key) { }
	g_buttons = g_key_buttons = g_level_down = g_level_pressed = 0;
	g_volume_as[0] = g_volume_as[1] = 0;
	const Uint8 *keys = SDL_GetKeyboardState(NULL);
	for (size_t i = 0; i < sizeof keymap / sizeof keymap[0]; i++)
		if (keys[keymap[i].key]) g_key_buttons |= DIATOM_BIT(keymap[i].btn);
	g_menu_down = keys[SDL_SCANCODE_TAB] || keys[SDL_SCANCODE_ESCAPE] ||
		(g_pad && SDL_JoystickGetButton(g_pad, 8));
	if (keys[SDL_SCANCODE_EQUALS] || (g_pad && SDL_JoystickGetButton(g_pad, 128)))
		volume_event(0, true);
	if (keys[SDL_SCANCODE_MINUS] || (g_pad && SDL_JoystickGetButton(g_pad, 129)))
		volume_event(1, true);
	for (int i = 0; i < g_nkeys; i++) {
		unsigned char bits[KEY_MAX / 8 + 1] = { 0 };
		if (ioctl(g_keys[i], EVIOCGKEY(sizeof bits), bits) < 0) continue;
		if (bits[KEY_VOLUMEUP / 8] & (1 << (KEY_VOLUMEUP % 8))) volume_event(0, true);
		if (bits[KEY_VOLUMEDOWN / 8] & (1 << (KEY_VOLUMEDOWN % 8))) volume_event(1, true);
	}
}

bool     diatom_port_should_quit(void) { return g_quit; }

bool diatom_port_capture(const char *path)
{
	SDL_Surface *s;
	int w, h;
	bool ok;

	if (!g_renderer || !path) return false;
	SDL_GetRendererOutputSize(g_renderer, &w, &h);

	s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
	if (!s) return false;

	/* Read back what was actually presented, so the capture shows the real
	 * scaling and letterboxing rather than the core's raw framebuffer. */
	if (SDL_RenderReadPixels(g_renderer, NULL, SDL_PIXELFORMAT_ARGB8888,
	                         s->pixels, s->pitch) != 0) {
		SDL_FreeSurface(s);
		return false;
	}
	/* Save as plain 24-bit RGB. A 32-bit BMP carries a V4/V5 header with alpha
	 * masks that several readers - macOS ImageIO among them - refuse, and the
	 * alpha channel is meaningless here anyway. */
	{
		SDL_Surface *rgb = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_RGB24, 0);
		SDL_FreeSurface(s);
		if (!rgb) return false;
		ok = SDL_SaveBMP(rgb, path) == 0;
		SDL_FreeSurface(rgb);
	}
	return ok;
}

uint64_t diatom_port_now_us(void)
{
	return diatom_ticks_to_us(SDL_GetPerformanceCounter(),
	                          SDL_GetPerformanceFrequency());
}

void diatom_port_log(diatom_log_level lvl, const char *msg)
{
	static const char *tag[] = { "debug", "info", "warn", "error" };
	fprintf(stderr, "[%s] %s\n", tag[lvl], msg);
}

static snd_mixer_elem_t *control(const char *name)
{
	for (snd_mixer_elem_t *e = g_mixer ? snd_mixer_first_elem(g_mixer) : NULL;
	     e; e = snd_mixer_elem_next(e))
		if (!strcmp(snd_mixer_selem_get_name(e), name) &&
		    snd_mixer_selem_has_playback_volume(e)) return e;
	return NULL;
}

static void settings_open(void)
{
	g_disp = open("/dev/disp", O_RDWR | O_CLOEXEC);
	if (snd_mixer_open(&g_mixer, 0) >= 0 &&
	    (snd_mixer_attach(g_mixer, "default") < 0 ||
	     snd_mixer_selem_register(g_mixer, NULL, NULL) < 0 || snd_mixer_load(g_mixer) < 0)) {
		snd_mixer_close(g_mixer); g_mixer = NULL;
	}
	FILE *f = fopen("/sys/class/gpio/gpio243/value", "r");
	int value = 0;
	if (f) { if (fscanf(f, "%d", &value) != 1) value = 0; fclose(f); }
	g_fn = g_muted = value != 0;
}

static void settings_audio(void)
{
	snd_mixer_elem_t *hp = control("Headphone");
	if (hp) snd_mixer_selem_set_playback_volume_all(hp, 0);
	for (snd_mixer_elem_t *e = g_mixer ? snd_mixer_first_elem(g_mixer) : NULL;
	     e; e = snd_mixer_elem_next(e))
		if (!strcmp(snd_mixer_selem_get_name(e), "DAC Swap") && snd_mixer_selem_is_enumerated(e))
			snd_mixer_selem_set_enum_item(e, SND_MIXER_SCHN_FRONT_LEFT, 0);
	diatom_port_mute_set(g_muted);
}

static void volume_apply(void)
{
	snd_mixer_elem_t *digital = control("digital volume"), *dac = control("DAC volume");
	long lo, hi;
	if (g_volume < 0 || !digital || !dac) return;
	snd_mixer_selem_get_playback_volume_range(digital, &lo, &hi);
	snd_mixer_selem_set_playback_volume_all(digital, hi - (hi - lo) * g_volume / 20);
	snd_mixer_selem_get_playback_volume_range(dac, &lo, &hi);
	snd_mixer_selem_set_playback_volume_all(dac,
		g_muted || g_volume == 0 ? lo : (hi < 160 ? hi : 160));
}

int diatom_port_level_positions(diatom_level_kind kind)
{
	if (kind == DIATOM_LEVEL_VOLUME)
		return control("digital volume") && control("DAC volume") ? 21 : 0;
	if (kind == DIATOM_LEVEL_BRIGHTNESS) return g_disp >= 0 ? 12 : 0;
	return 0;
}

int diatom_port_level_read(diatom_level_kind kind)
{
	if (!diatom_port_level_positions(kind)) return -1;
	if (kind == DIATOM_LEVEL_VOLUME) {
		snd_mixer_elem_t *e = control("digital volume");
		long lo, hi, raw;
		snd_mixer_handle_events(g_mixer);
		snd_mixer_selem_get_playback_volume_range(e, &lo, &hi);
		if (hi <= lo || snd_mixer_selem_get_playback_volume(e, SND_MIXER_SCHN_FRONT_LEFT, &raw) < 0)
			return -1;
		g_volume = (int)(((hi - raw) * 20 + (hi - lo) / 2) / (hi - lo));
		if (g_volume < 0) g_volume = 0;
		if (g_volume > 20) g_volume = 20;
		return g_volume;
	}
	unsigned long args[4] = { 0, 0, 0, 0 };
	int raw = ioctl(g_disp, 0x103, args);
	return raw < 0 ? -1 : ladder_nearest(bright_ladder, 12, raw);
}

void diatom_port_level_write(diatom_level_kind kind, int pos)
{
	int count = diatom_port_level_positions(kind);
	if (!count) return;
	if (pos < 0) pos = 0;
	if (pos >= count) pos = count - 1;
	if (kind == DIATOM_LEVEL_VOLUME) { g_volume = pos; volume_apply(); }
	else {
		unsigned long args[4] = { 0, bright_ladder[pos], 0, 0 };
		ioctl(g_disp, 0x102, args);
	}
}

uint32_t diatom_port_level_keys(void)
{
	uint32_t keys = g_level_down | g_level_pressed;
	g_level_pressed = 0;
	return keys;
}
void diatom_port_level_shown(diatom_level_kind kind, int pos, int positions)
{
	g_osd_kind = kind; g_osd_pos = pos; g_osd_count = positions;
	g_osd_until = diatom_port_now_us() + 1500000ull;
}
void diatom_port_level_invalidate(void) { g_volume = -1; }

void diatom_port_low_battery(bool on, bool blink) { g_low = on; g_blink = blink; }

static void draw_indicators(void)
{
	if (g_osd_count > 1 && diatom_port_now_us() < g_osd_until) {
		SDL_Rect bg = { 0, 0, WINDOW_W, 12 };
		SDL_Rect bar = { 0, 3, WINDOW_W * g_osd_pos / (g_osd_count - 1), 6 };
		SDL_SetRenderDrawColor(g_renderer, 24, 24, 24, 255);
		SDL_RenderFillRect(g_renderer, &bg);
		SDL_SetRenderDrawColor(g_renderer, g_osd_kind == DIATOM_LEVEL_BRIGHTNESS ? 240 : 72,
		                       192, 224, 255);
		SDL_RenderFillRect(g_renderer, &bar);
	}
	if (g_low && (!g_blink || (diatom_port_now_us() / 1000000ull) % 2 == 0)) {
		SDL_SetRenderDrawColor(g_renderer, 224, 72, 72, 255);
		for (int y = -9; y <= 9; y++)
			for (int x = -9; x <= 9; x++)
				if (x*x + y*y <= 81)
					SDL_RenderDrawPoint(g_renderer, WINDOW_W - 34 + x, 34 + y);
	}
	SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
}

void diatom_port_present_stop(diatom_park park)
{
	if (!g_presented) return;
	if (park == DIATOM_PARK_BLANK) {
		SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
		SDL_RenderClear(g_renderer);
		SDL_RenderPresent(g_renderer);
	}
	void (*finish)(void) = SDL_GL_GetProcAddress("glFinish");
	if (finish) finish();
	g_presented = false;
}

void diatom_port_mute_set(bool on)
{
	if (g_volume < 0) diatom_port_level_read(DIATOM_LEVEL_VOLUME);
	g_muted = on;
	volume_apply();
}
bool diatom_port_mute_get(void) { return g_muted; }
void diatom_port_fast_clock(bool on) { (void)on; }
