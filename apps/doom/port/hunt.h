// SPDX-License-Identifier: GPL-2.0-or-later
/* The hunt (port/hunt.c): with env DOOM_HUNT=1 every demon on the map is a pod */
#ifndef DOOM_HUNT_H
#define DOOM_HUNT_H

#include <stdint.h>

struct mobj_s;
struct vissprite_s;

extern int hunt_on;

void hunt_init(void);
/* An event on a topic of the hunt; 0 for any other topic */
int hunt_event(const char* topic, uint32_t topic_len, const char* msg, uint32_t len);
/* After the tics of a k32_tick: demons come and go, kills go out */
void hunt_tick(int64_t now_ms);
/* Before a frame is drawn: what the player aims at */
void hunt_frame(int64_t now_ms);
/* A request of the hunt: an HTTP status once answered, 0 for the others */
int32_t hunt_route(const char* method, const char* seg);

/* The engine calls these while hunt_on */
void hunt_killed(struct mobj_s* target);
int hunt_shielded(const struct mobj_s* target, const struct mobj_s* source);
int hunt_shot(struct mobj_s* shooter);
void hunt_project(const struct mobj_s* thing, const struct vissprite_s* vis);
void hunt_sprite_drawn(const struct vissprite_s* vis, const short* floorclip, const short* ceilingclip);

/* After the status bar's record in a draw list: the names to show, 256 bytes at most */
#define HUNT_RECORD_MAX 256
uint32_t hunt_record(uint8_t* p);

#endif
