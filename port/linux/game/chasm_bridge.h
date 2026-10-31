/*
CHASM_BRIDGE.H

The OpenCE chasm bridge's game half (chasm_bridge.c): the NPC the player is
looking at, the talk key, the reply on the HUD, and the basic in-world
actions the backend may ask for.

The bridge is off unless bridge.enabled is true (config.toml, or
HALO_BRIDGE=1). It is single-player campaign only: a network client has no
actor datums to act on.
*/

#ifndef __CHASM_BRIDGE_H
#define __CHASM_BRIDGE_H
#pragma once

/* on every frame (main.c): the talk key, the worker's answer, the heartbeat */
void chasm_bridge_update(void);
/* on a map's load and its dispose (game.c) */
void chasm_bridge_initialize_for_new_map(void);
void chasm_bridge_dispose_from_old_map(void);

#endif
