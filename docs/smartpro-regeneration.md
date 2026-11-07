# Smart Pro port regeneration

New port based on pinned upstream c52c347daa65e13d87122e874f4d7b6261fa815a,
retained offline runtime contracts and earlier successful patch records. This is
not an identical recovery of the lost final source. TG5040 Smart Pro only.

`make PORT=smartpro` selects SDL2/ALSA, a1280x720 surface, Sunxi display
brightness ioctl, the codec mixer and the existing GPIO243 mute input. Runtime
SDL/DDK/kernel remain separately pinned; native dummy drivers do not test them.
No GPIO/DTB workaround is added for the existing controller GPIO107 warning.

The logical pad remains B,A,Y,X on joystick indices0,1,2,3. Do not use the
launcher's reported A/B label mismatch as authority to swap emulated buttons.
MENU is held state plus a down pulse; host policy consumes edges after every
emulated frame, including frames hidden by fast-forward. Pause is deferred until
the batch finishes. Level chords count at endpoints and on same-poll releases.

`make check-smartpro` uses real SDL virtual joystick and key events, dummy
display/audio, and no ROMs. It checks logical face buttons and short MENU/level
taps. `make check` additionally covers protocol, state plane and desktop audio.
On ARM64 the pinned cheevos-test target has a pre-existing missing libm link;
`CC='cc -Wl,--no-as-needed -lm'` is a disclosed verification workaround, not a
plain stock suite pass. Target playback, handoff, audio and saves remain hardware
gates. No owner games or private donor binaries belong in this repository.
