/*
AI_PLAYER.H

The Gem AI co-op player's input, produced in port/linux/game/ai_player.c and
applied to the local player's input blob in player_control.c. It only runs in
the AI instance (ai_player.enabled); the human instance leaves it off.
*/

#ifndef __AI_PLAYER_H
#define __AI_PLAYER_H
#pragma once

#include "cseries.h"

struct ai_player_input
{
	real throttle_i;
	real throttle_j;
	real facing_delta_yaw;
	real facing_delta_pitch;
	real primary_trigger;
	/* _unit_control_* flags: firing, reloading, and later the action button */
	unsigned long unit_control_flags;
};

/* 1 if it filled input, 0 if the bot is off, or there is no local player or
   teammate to follow. */
int ai_player_get_input(short local_player_index, struct ai_player_input *input);

/* the testing console command ("gembot ..."): 1 if it handled the line */
boolean ai_player_console_command(char const *command);

#endif
