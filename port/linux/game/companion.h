#ifndef __COMPANION_H
#define __COMPANION_H

/* Kevin, the campaign's red teammate (companion.c): placed beside the
player, kept following him; a tick's step of game_tick */
void companion_update(
	void);
/* the icon of the microphone Kevin hears the player by, open or muted
(interface.c, after the HUD) */
void companion_draw_mic(
	void);
/* "kevin <text>" typed in the console (console.c): the text goes to mate as a
game master's line; FALSE if the command is something else */
boolean companion_console_command(
	char const *command);

/* mate keeps Kevin's memory with the game's saves (game_state.c): a checkpoint
saved, and a saved game loaded or gone back to */
void companion_note_saved(
	void);
void companion_note_loaded(
	void);

/* Kevin's eyes (d3d8_gl.c): whether mate asked for the player's view (once),
and the frame taken as it was shown, as RGB rows from the top, which goes to
mate with the spots marked on it */
boolean companion_view_wanted(
	void);
void companion_view_taken(
	unsigned char const *rgb,
	unsigned long width,
	unsigned long height);

/* a sound beginning to play (sound_manager.c): Kevin hears the dialogue */
struct sound_permutation;
void companion_note_sound(
	long definition_index,
	struct sound_permutation const *permutation,
	long source_identifier);

#endif
