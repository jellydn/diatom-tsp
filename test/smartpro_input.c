/* New public-port regression; no device GPIO, display or game required. */
#include <SDL.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "diatom_port.h"

static void key(Uint32 type, SDL_Keycode sym)
{
	SDL_Event e = { 0 };
	e.type = type;
	e.key.keysym.sym = sym;
	assert(SDL_PushEvent(&e) == 1);
}

int main(void)
{
	alarm(15);
	setenv("SDL_VIDEODRIVER", "dummy", 1);
	setenv("SDL_AUDIODRIVER", "dummy", 1);
	setenv("SDL_RENDER_DRIVER", "software", 1);
	setenv("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1", 1);
	assert(SDL_Init(SDL_INIT_JOYSTICK | SDL_INIT_EVENTS) == 0);
	int index = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 6, 130, 1);
	assert(index >= 0);
	SDL_Joystick *pad = SDL_JoystickOpen(index);
	assert(pad);
	diatom_port_caps caps;
	assert(diatom_port_init(&caps));
	assert(caps.surface_w == 1280 && caps.surface_h == 720);
	for (int axis = 0; axis < 6; axis++)
		assert(SDL_JoystickSetVirtualAxis(pad, axis, 0) == 0);
	diatom_port_input_reset();
	const int expected[] = { DIATOM_BTN_B, DIATOM_BTN_A, DIATOM_BTN_Y, DIATOM_BTN_X };
	for (int button = 0; button < 4; button++) {
		assert(SDL_JoystickSetVirtualButton(pad, button, 1) == 0);
		diatom_port_input_poll();
		assert(diatom_port_input_state() == DIATOM_BIT(expected[button]));
		assert(SDL_JoystickSetVirtualButton(pad, button, 0) == 0);
		diatom_port_input_poll();
		assert(diatom_port_input_state() == 0);
	}
	/* Same-poll taps must survive until the host consumes this frame. */
	key(SDL_KEYDOWN, SDLK_TAB);
	key(SDL_KEYUP, SDLK_TAB);
	diatom_port_input_poll();
	assert(diatom_port_input_state() == DIATOM_BIT(DIATOM_BTN_MENU));
	diatom_port_input_poll();
	assert(diatom_port_input_state() == 0);
	key(SDL_KEYDOWN, SDLK_TAB);
	key(SDL_KEYDOWN, SDLK_EQUALS);
	key(SDL_KEYUP, SDLK_EQUALS);
	key(SDL_KEYUP, SDLK_TAB);
	diatom_port_input_poll();
	assert(diatom_port_level_keys() == DIATOM_LEVEL_KEY_BRIGHTNESS_UP);
	assert(diatom_port_level_keys() == 0);
	diatom_port_input_poll();
	key(SDL_KEYDOWN, SDLK_MINUS);
	diatom_port_input_poll();
	assert(diatom_port_level_keys() == DIATOM_LEVEL_KEY_VOLUME_DOWN);
	assert(diatom_port_level_keys() == DIATOM_LEVEL_KEY_VOLUME_DOWN);
	key(SDL_KEYUP, SDLK_MINUS);
	diatom_port_input_poll();
	assert(diatom_port_level_keys() == 0);
	diatom_port_shutdown();
	SDL_JoystickClose(pad);
	SDL_JoystickDetachVirtual(index);
	SDL_Quit();
	puts("PASS: logical B/A/Y/X unchanged; MENU tap and level chords retained");
	return 0;
}
