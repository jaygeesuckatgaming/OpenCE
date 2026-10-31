/*
MATE_LINK.H

The link to the "mate" voice program of the campaign's teammate
(mate_link.c). The game's side is port/linux/game/companion.c, which calls
these as ints; dsound_sdl.c plays the voice, a stream of its own.
*/

#ifndef MATE_LINK_H
#define MATE_LINK_H

/* starts connecting (once; again whenever the connection drops) */
void mate_link_start(void);
int mate_link_connected(void);
/* the next JSON message from mate, if any; 1 if one was copied */
int mate_link_next_message(char *buffer, int size);
/* sends a JSON message to mate (dropped while not connected) */
void mate_link_send(const char *json);
/* sends dialogue the game played for mate to hear: what it says of it (JSON),
and its samples (mono, 16-bit) */
void mate_link_send_hear(const char *json, const short *samples, unsigned long count);
/* sends the player's view mate asked for: what companion.c marks on it (JSON),
and its RGB pixels, rows from the top */
void mate_link_send_view(const char *json, const unsigned char *rgb, unsigned long size);
/* how much Kevin's voice sounds over the radio (0 to 1: band-passed, gritty,
static as he keys it and lets go) */
void mate_link_set_radio(float radio);
/* his voice's DirectSound stream (NULL until started): a 3D voice of the
mixer's, which companion.c places as the game places its sounds */
struct IDirectSoundStream *mate_link_voice_stream(void);
/* whether Kevin's voice is playing */
int mate_link_voice_playing(void);
/* (dsound_sdl.c, on the audio thread, before it mixes `frames`) feeds his
voice into its stream */
void mate_link_feed(unsigned long frames);

#endif
