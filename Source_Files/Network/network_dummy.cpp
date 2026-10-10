/*

	Copyright (C) 1991-2001 and beyond by Bungie Studios, Inc.
	and the "Aleph One" developers.
 
	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	This license is contained in the file "COPYING",
	which is included with this source code; it is available online at
	http://www.gnu.org/licenses/gpl.html

*/
/*
/*
 *  network_dummy.cpp - Network entry points for builds without network play
 *
 *  Built instead of the network, metaserver and TCPMess sources when
 *  configured with --disable-network (A1_NO_NETWORK). Callers outside the
 *  network subsystem only reach these when game_is_networked is set, which
 *  never happens in such a build; the NetAllow* answers and chat prompt match
 *  what the real code returns for a local game. One exception:
 *  display_net_game_stats() also ends the replay of a multiplayer film, so
 *  those replays return to the menu without the postgame carnage report.
 */

#include "cseries.h"
#include "map.h"
#include "player.h"
#include "preferences.h"
#include "network.h"
#include "interface.h"
#include "HTTP.h"

// network.cpp

static const NetworkStats sInvalidStats = {
	NetworkStats::invalid,
	NetworkStats::invalid,
	NetworkStats::invalid,
	0
};

void NetSetChatCallbacks(ChatCallbacks *) { }
void ChatCallbacks::SendChatMessage(const std::string&) { }

InGameChatCallbacks *InGameChatCallbacks::instance()
{
	static InGameChatCallbacks *m_instance = new InGameChatCallbacks();
	return m_instance;
}

std::string InGameChatCallbacks::prompt()
{
	return (std::string(player_preferences->name) + ":");
}

void InGameChatCallbacks::ReceivedMessageFromPlayer(const char *, const char *) { }

void NetExit(void) { }
bool NetSync(void) { return true; }
bool NetUnSync(void) { return true; }
bool NetStart(void) { return false; }
std::string NetSessionIdentifier(void) { return std::string(); }
short NetGetLocalPlayerIndex(void) { return 0; }
short NetGetNumberOfPlayers(void) { return 1; }
void *NetGetGameData(void) { return NULL; }
int32 NetGetNetTime(void) { return 0; }
bool NetChangeMap(struct entry_point *) { return false; }
void NetProcessMessagesInGame() { }
bool NetCheckWorldUpdate() { return false; }
void NetSetupTopologyFromStarts(const player_start_data *, short) { }
void NetSetResumedGameWadForRemoteHub(byte *, int) { }
OSErr NetDistributeGameDataToAllPlayers(byte *, int32, bool, CommunicationsChannel *) { return 1; }
byte *NetReceiveGameData(bool) { return NULL; }

void construct_multiplayer_starts(player_start_data *, short *outStartCount)
{
	if (outStartCount) *outStartCount = 0;
}

int32 NetGetLatency() { return NetworkStats::invalid; }
const NetworkStats& NetGetStats(int) { return sInvalidStats; }
int32 NetGetUnconfirmedActionFlagsCount() { return 0; }
uint32 NetGetUnconfirmedActionFlag(int32) { return 0; }
void NetUpdateUnconfirmedActionFlags() { }

// Same rules as network.cpp; there is no remote hub and the local machine
// always counts as the server.
bool NetAllowCrosshair() {
	return (dynamic_world->player_count == 1 ||
		(dynamic_world->game_information.cheat_flags & _allow_crosshair));
}

bool NetAllowTunnelVision() {
	return (dynamic_world->player_count == 1 ||
		dynamic_world->game_information.cheat_flags & _allow_tunnel_vision);
}

bool NetAllowBehindview() {
	return (dynamic_world->player_count == 1 ||
		dynamic_world->game_information.cheat_flags & _allow_behindview);
}

bool NetAllowCarnageMessages() {
	return (dynamic_world->player_count == 1 ||
		!(dynamic_world->game_information.cheat_flags & _disable_carnage_messages));
}

bool NetAllowSavingLevel() {
	return true;
}

bool NetAllowOverlayMap() {
	return (dynamic_world->player_count == 1 ||
		(dynamic_world->game_information.cheat_flags & _allow_overlay_map));
}

// network_star_hub.cpp (the "latency tolerance" console command)

static int32 sMinimumSendPeriod = 3; // kDefaultMinimumSendPeriod
int32& hub_get_minimum_send_period() { return sMinimumSendPeriod; }
void hub_set_minimum_send_period(int32 new_minimum) { sMinimumSendPeriod = new_minimum; }

// network_dialogs.cpp

void display_net_game_stats(void) { }
bool network_gather(bool, bool&) { return false; }
int network_join(void) { return kNetworkJoinFailedUnjoined; }

// HTTP.cpp

void HTTPClient::Init() { }
bool HTTPClient::Get(const std::string&) { return false; }
bool HTTPClient::Post(const std::string&, const parameter_map&) { return false; }
