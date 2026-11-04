// SPDX-License-Identifier: GPL-2.0-or-later
/* Two players in lockstep (port/net.c) */
#ifndef DOOM_NET_H
#define DOOM_NET_H

#include <stdint.h>

#define NET_SINGLE 0
#define NET_AUTHORITY 1
#define NET_CLIENT 2

int net_role(void);
/* The pod orders the ticcmds: args and iwad are what a client's engine starts with */
void net_init_authority(const char* args, const char* iwad);
/* Runs the tics due by now whose ticcmds are in; returns the tics run */
int net_tick(int64_t now_ms);
/* The netgame's requests: an HTTP status once answered, 0 for the others */
int32_t net_route(const char* method, const char* seg, const char* body);

#endif
