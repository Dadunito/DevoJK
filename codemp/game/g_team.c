/*
===========================================================================
Copyright (C) 1999 - 2005, Id Software, Inc.
Copyright (C) 2000 - 2013, Raven Software, Inc.
Copyright (C) 2001 - 2013, Activision, Inc.
Copyright (C) 2013 - 2015, OpenJK contributors

This file is part of the OpenJK source code.

OpenJK is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License version 2 as
published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, see <http://www.gnu.org/licenses/>.
===========================================================================
*/

#include "g_local.h"
#include "bg_saga.h"

typedef struct teamgame_s {
	float			last_flag_capture;
	int				last_capture_team;
	flagStatus_t	redStatus;	// CTF
	flagStatus_t	blueStatus;	// CTF
	flagStatus_t	flagStatus;	// One Flag CTF
	int				redTakenTime;
	int				blueTakenTime;
} teamgame_t;

teamgame_t teamgame;

void Team_SetFlagStatus( int team, flagStatus_t status );

void Team_InitGame( void ) {
	memset(&teamgame, 0, sizeof teamgame);

	switch( level.gametype ) {
	case GT_CTF:
	case GT_CTY:
		teamgame.redStatus = -1; // Invalid to force update
		Team_SetFlagStatus( TEAM_RED, FLAG_ATBASE );
		teamgame.blueStatus = -1; // Invalid to force update
		Team_SetFlagStatus( TEAM_BLUE, FLAG_ATBASE );
		break;
	default:
		break;
	}
}

int OtherTeam(int team) {
	if (team==TEAM_RED)
		return TEAM_BLUE;
	else if (team==TEAM_BLUE)
		return TEAM_RED;
	return team;
}

const char *TeamName(int team)  {
	if (team==TEAM_RED)
		return "RED";
	else if (team==TEAM_BLUE)
		return "BLUE";
	else if (team==TEAM_SPECTATOR)
		return "SPECTATOR";
	return "FREE";
}

const char *OtherTeamName(int team) {
	if (team==TEAM_RED)
		return "BLUE";
	else if (team==TEAM_BLUE)
		return "RED";
	else if (team==TEAM_SPECTATOR)
		return "SPECTATOR";
	return "FREE";
}

const char *TeamColorString(int team) {
	if (team==TEAM_RED)
		return S_COLOR_RED;
	else if (team==TEAM_BLUE)
		return S_COLOR_BLUE;
	else if (team==TEAM_SPECTATOR)
		return S_COLOR_YELLOW;
	return S_COLOR_WHITE;
}

// NULL for everyone
/*
void QDECL PrintMsg( gentity_t *ent, const char *fmt, ... ) {
	char		msg[1024];
	va_list		argptr;
	char		*p;

	va_start (argptr,fmt);
	if (vsprintf (msg, fmt, argptr) > sizeof(msg)) {
		trap->Error( ERR_DROP, "PrintMsg overrun" );
	}
	va_end (argptr);

	// double quotes are bad
	while ((p = strchr(msg, '"')) != NULL)
		*p = '\'';

	trap->SendServerCommand ( ( (ent == NULL) ? -1 : ent-g_entities ), va("print \"%s\"", msg ));
}
*/
//Printing messages to players via this method is no longer done, StringEd stuff is client only.


//plIndex used to print pl->client->pers.netname
//teamIndex used to print team name
void PrintCTFMessage(int plIndex, int teamIndex, int ctfMessage)
{
	gentity_t *te;

	if (plIndex == -1)
	{
		plIndex = MAX_CLIENTS+1;
	}
	if (teamIndex == -1)
	{
		teamIndex = 50;
	}

	te = G_TempEntity(vec3_origin, EV_CTFMESSAGE);
	te->r.svFlags |= SVF_BROADCAST;
	te->s.eventParm = ctfMessage;
	te->s.trickedentindex = plIndex;
	if (ctfMessage == CTFMESSAGE_PLAYER_CAPTURED_FLAG)
	{
		if (teamIndex == TEAM_RED)
		{
			te->s.trickedentindex2 = TEAM_BLUE;
		}
		else
		{
			te->s.trickedentindex2 = TEAM_RED;
		}
	}
	else
	{
		te->s.trickedentindex2 = teamIndex;
	}
}

/*
==============
AddTeamScore

 used for gametype > GT_TEAM
 for gametype GT_TEAM the level.teamScores is updated in AddScore in g_combat.c
==============
*/
void AddTeamScore(vec3_t origin, int team, int score, qboolean announceScore) {
	int eventParm;

	if ( team == TEAM_RED ) {
		if ( level.teamScores[ TEAM_RED ] + score == level.teamScores[ TEAM_BLUE ] ) {
			eventParm = GTS_TEAMS_ARE_TIED;//teams are tied sound
		}
		else if ( level.teamScores[ TEAM_RED ] <= level.teamScores[ TEAM_BLUE ] && level.teamScores[ TEAM_RED ] + score > level.teamScores[ TEAM_BLUE ]) {
			eventParm = GTS_REDTEAM_TOOK_LEAD;// red took the lead sound
		}
		else {
			eventParm = GTS_REDTEAM_SCORED;// red scored sound
		}
	}
	else {
		if ( level.teamScores[ TEAM_BLUE ] + score == level.teamScores[ TEAM_RED ] ) {
			eventParm = GTS_TEAMS_ARE_TIED;//teams are tied sound
		}
		else if ( level.teamScores[ TEAM_BLUE ] <= level.teamScores[ TEAM_RED ] && level.teamScores[ TEAM_BLUE ] + score > level.teamScores[ TEAM_RED ]) {
			eventParm = GTS_BLUETEAM_TOOK_LEAD;// blue took the lead sound
		}
		else {
			eventParm = GTS_BLUETEAM_SCORED;// blue scored sound
		}
	}

	if (announceScore || (eventParm != GTS_BLUETEAM_SCORED && eventParm != GTS_REDTEAM_SCORED)) { //Only announce score change if desired or important
		gentity_t	*te;

		te = G_TempEntity(origin, EV_GLOBAL_TEAM_SOUND );
		te->r.svFlags |= SVF_BROADCAST;

		te->s.eventParm = eventParm;
	}

	level.teamScores[ team ] += score;
}

/*
==============
OnSameTeam
==============
*/
qboolean OnSameTeam( gentity_t *ent1, gentity_t *ent2 ) {
	if ( !ent1->client || !ent2->client ) {
		return qfalse;
	}

	if (level.gametype == GT_POWERDUEL)
	{
		if (ent1->client->sess.duelTeam == ent2->client->sess.duelTeam)
		{
			return qtrue;
		}

		return qfalse;
	}

	if (level.gametype == GT_SINGLE_PLAYER)
	{
		qboolean ent1IsBot = qfalse;
		qboolean ent2IsBot = qfalse;

		if (ent1->r.svFlags & SVF_BOT)
		{
			ent1IsBot = qtrue;
		}
		if (ent2->r.svFlags & SVF_BOT)
		{
			ent2IsBot = qtrue;
		}

		if ((ent1IsBot && ent2IsBot) || (!ent1IsBot && !ent2IsBot))
		{
			return qtrue;
		}
		return qfalse;
	}

	if ( level.gametype < GT_TEAM ) {
		return qfalse;
	}

	if (ent1->s.eType == ET_NPC &&
		ent1->s.NPC_class == CLASS_VEHICLE &&
		ent1->client &&
		ent1->client->sess.sessionTeam != TEAM_FREE &&
		ent2->client &&
		ent1->client->sess.sessionTeam == ent2->client->sess.sessionTeam)
	{
		return qtrue;
	}
	if (ent2->s.eType == ET_NPC &&
		ent2->s.NPC_class == CLASS_VEHICLE &&
		ent2->client &&
		ent2->client->sess.sessionTeam != TEAM_FREE &&
		ent1->client &&
		ent2->client->sess.sessionTeam == ent1->client->sess.sessionTeam)
	{
		return qtrue;
	}

	if (ent1->client->sess.sessionTeam == TEAM_FREE &&
		ent2->client->sess.sessionTeam == TEAM_FREE &&
		ent1->s.eType == ET_NPC &&
		ent2->s.eType == ET_NPC)
	{ //NPCs don't do normal team rules
		return qfalse;
	}

	if (ent1->s.eType == ET_NPC && ent2->s.eType == ET_PLAYER)
	{
		return qfalse;
	}
	else if (ent1->s.eType == ET_PLAYER && ent2->s.eType == ET_NPC)
	{
		return qfalse;
	}

	if ( ent1->client->sess.sessionTeam == ent2->client->sess.sessionTeam ) {
		return qtrue;
	}

	return qfalse;
}

static char ctfFlagStatusRemap[] = { '0', '1', '*', '*', '2' };

void Team_SetFlagStatus( int team, flagStatus_t status ) {
	qboolean modified = qfalse;

	switch( team ) {
	case TEAM_RED:	// CTF
		if( teamgame.redStatus != status ) {
			teamgame.redStatus = status;
			modified = qtrue;
		}
		break;

	case TEAM_BLUE:	// CTF
		if( teamgame.blueStatus != status ) {
			teamgame.blueStatus = status;
			modified = qtrue;
		}
		break;

	case TEAM_FREE:	// One Flag CTF
		if( teamgame.flagStatus != status ) {
			teamgame.flagStatus = status;
			modified = qtrue;
		}
		break;
	}

	if( modified ) {
		char st[4] = { 0 };

		if( level.gametype == GT_CTF || level.gametype == GT_CTY ) {
			if (g_neutralFlag.integer) {
				return;//IDK, bugged,  Fix this?,  It sends a weird configstring to clients when used with neutralflag?
			}
			else {
				st[0] = ctfFlagStatusRemap[teamgame.redStatus];
				st[1] = ctfFlagStatusRemap[teamgame.blueStatus];
				st[2] = 0;
			}
		}
		else {
			st[0] = ctfFlagStatusRemap[teamgame.flagStatus];
			st[1] = 0;
		}
		trap->SetConfigstring( CS_FLAGSTATUS, st );
	}
}

void Team_CheckDroppedItem( gentity_t *dropped ) {
	if( dropped->item->giTag == PW_REDFLAG ) {
		Team_SetFlagStatus( TEAM_RED, FLAG_DROPPED );
	}
	else if( dropped->item->giTag == PW_BLUEFLAG ) {
		Team_SetFlagStatus( TEAM_BLUE, FLAG_DROPPED );
	}
	else if( dropped->item->giTag == PW_NEUTRALFLAG ) {
		Team_SetFlagStatus( TEAM_FREE, FLAG_DROPPED );
	}
}

/*
================
Team_FragBonuses

Calculate the bonuses for flag defense, flag carrier defense, etc.
Note that bonuses are not cumulative.  You get one, they are in importance
order.
================
*/
void Team_FragBonuses(gentity_t *targ, gentity_t *inflictor, gentity_t *attacker)
{
	int i;
	gentity_t *ent;
	int flag_pw, enemy_flag_pw;
	int otherteam;
	gentity_t *flag, *carrier = NULL;
	char *c;
	vec3_t v1, v2;
	int team;

	// no bonus for fragging yourself or team mates
	if (!targ->client || !attacker->client || targ == attacker || OnSameTeam(targ, attacker))
		return;

	team = targ->client->sess.sessionTeam;
	otherteam = OtherTeam(targ->client->sess.sessionTeam);
	if (otherteam < 0)
		return; // whoever died isn't on a team

	// same team, if the flag at base, check to he has the enemy flag
	if (team == TEAM_RED) {
		flag_pw = PW_REDFLAG;
		enemy_flag_pw = PW_BLUEFLAG;
	} else if (team == TEAM_BLUE){
		flag_pw = PW_BLUEFLAG;
		enemy_flag_pw = PW_REDFLAG;
	} else { //rabbit ?
		flag_pw = PW_NEUTRALFLAG;
		enemy_flag_pw = PW_NEUTRALFLAG;
	}

	// did the attacker frag the flag carrier?
	if (targ->client->ps.powerups[enemy_flag_pw]) {
		attacker->client->pers.teamState.lastfraggedcarrier = level.time;
		if (g_fixCTFScores.integer)
			AddScore(attacker, targ->r.currentOrigin, 4);
		else
			AddScore(attacker, targ->r.currentOrigin, CTF_FRAG_CARRIER_BONUS);
		attacker->client->pers.teamState.fragcarrier++;
		if (g_fixCTFScores.integer)
			attacker->client->ps.persistant[PERS_DEFEND_COUNT]++;
		//PrintMsg(NULL, "%s" S_COLOR_WHITE " fragged %s's flag carrier!\n",
		//	attacker->client->pers.netname, TeamName(team));
		PrintCTFMessage(attacker->s.number, team, CTFMESSAGE_FRAGGED_FLAG_CARRIER);

		// the target had the flag, clear the hurt carrier
		// field on the other team
		for (i = 0; i < sv_maxclients.integer; i++) {
			ent = g_entities + i;
			if (ent->inuse && ent->client->sess.sessionTeam == otherteam)
				ent->client->pers.teamState.lasthurtcarrier = 0;
		}
		return;
	}

	if (targ->client->pers.teamState.lasthurtcarrier &&
		level.time - targ->client->pers.teamState.lasthurtcarrier < CTF_CARRIER_DANGER_PROTECT_TIMEOUT &&
		!attacker->client->ps.powerups[flag_pw]) {
		// attacker is on the same team as the flag carrier and
		// fragged a guy who hurt our flag carrier
		if (g_fixCTFScores.integer)
			AddScore(attacker, targ->r.currentOrigin, 1);
		else
			AddScore(attacker, targ->r.currentOrigin, CTF_CARRIER_DANGER_PROTECT_BONUS);

		attacker->client->pers.teamState.carrierdefense++;
		targ->client->pers.teamState.lasthurtcarrier = 0;

		attacker->client->ps.persistant[PERS_DEFEND_COUNT]++;
		team = attacker->client->sess.sessionTeam;
		attacker->client->rewardTime = level.time + REWARD_SPRITE_TIME;

		return;
	}

	if (targ->client->pers.teamState.lasthurtcarrier &&
		level.time - targ->client->pers.teamState.lasthurtcarrier < CTF_CARRIER_DANGER_PROTECT_TIMEOUT) {
		// attacker is on the same team as the skull carrier and
		if (g_fixCTFScores.integer)
			AddScore(attacker, targ->r.currentOrigin, 1);
		else
			AddScore(attacker, targ->r.currentOrigin, CTF_CARRIER_DANGER_PROTECT_BONUS);

		attacker->client->pers.teamState.carrierdefense++;
		targ->client->pers.teamState.lasthurtcarrier = 0;

		attacker->client->ps.persistant[PERS_DEFEND_COUNT]++;
		team = attacker->client->sess.sessionTeam;
		attacker->client->rewardTime = level.time + REWARD_SPRITE_TIME;

		return;
	}

	// flag and flag carrier area defense bonuses

	// we have to find the flag and carrier entities

	// find the flag
	switch (attacker->client->sess.sessionTeam) {
	case TEAM_RED:
		c = "team_CTF_redflag";
		break;
	case TEAM_BLUE:
		c = "team_CTF_blueflag";
		break;
	default:
		return;
	}
	// find attacker's team's flag carrier
	for (i = 0; i < sv_maxclients.integer; i++) {
		carrier = g_entities + i;
		if (carrier->inuse && carrier->client->ps.powerups[flag_pw])
			break;
		carrier = NULL;
	}
	flag = NULL;
	while ((flag = G_Find (flag, FOFS(classname), c)) != NULL) {
		if (!(flag->flags & FL_DROPPED_ITEM))
			break;
	}

	if (!flag)
		return; // can't find attacker's flag

	// ok we have the attackers flag and a pointer to the carrier

	// check to see if we are defending the base's flag
	VectorSubtract(targ->r.currentOrigin, flag->r.currentOrigin, v1);
	VectorSubtract(attacker->r.currentOrigin, flag->r.currentOrigin, v2);

	if ( ( ( VectorLength(v1) < CTF_TARGET_PROTECT_RADIUS &&
		trap->InPVS(flag->r.currentOrigin, targ->r.currentOrigin ) ) ||
		( VectorLength(v2) < CTF_TARGET_PROTECT_RADIUS &&
		trap->InPVS(flag->r.currentOrigin, attacker->r.currentOrigin ) ) ) &&
		attacker->client->sess.sessionTeam != targ->client->sess.sessionTeam) {

		// we defended the base flag
		if (g_fixCTFScores.integer)
			AddScore(attacker, targ->r.currentOrigin, 1);
		else
			AddScore(attacker, targ->r.currentOrigin, CTF_FLAG_DEFENSE_BONUS);
		attacker->client->pers.teamState.basedefense++;

		attacker->client->ps.persistant[PERS_DEFEND_COUNT]++;
		attacker->client->rewardTime = level.time + REWARD_SPRITE_TIME;

		return;
	}

	if (carrier && carrier != attacker) {
		VectorSubtract(targ->r.currentOrigin, carrier->r.currentOrigin, v1);
		VectorSubtract(attacker->r.currentOrigin, carrier->r.currentOrigin, v1);

		if ( ( ( VectorLength(v1) < CTF_ATTACKER_PROTECT_RADIUS &&
			trap->InPVS(carrier->r.currentOrigin, targ->r.currentOrigin ) ) ||
			( VectorLength(v2) < CTF_ATTACKER_PROTECT_RADIUS &&
				trap->InPVS(carrier->r.currentOrigin, attacker->r.currentOrigin ) ) ) &&
			attacker->client->sess.sessionTeam != targ->client->sess.sessionTeam) {
			if (g_fixCTFScores.integer)
				AddScore(attacker, targ->r.currentOrigin, 1);
			else
				AddScore(attacker, targ->r.currentOrigin, CTF_CARRIER_PROTECT_BONUS);
			attacker->client->pers.teamState.carrierdefense++;

			attacker->client->ps.persistant[PERS_DEFEND_COUNT]++;
			attacker->client->rewardTime = level.time + REWARD_SPRITE_TIME;

			return;
		}
	}
}

/*
================
Team_CheckHurtCarrier

Check to see if attacker hurt the flag carrier.  Needed when handing out bonuses for assistance to flag
carrier defense.
================
*/
void Team_CheckHurtCarrier(gentity_t *targ, gentity_t *attacker)
{
	int flag_pw;

	if (!targ->client || !attacker->client)
		return;

	if (targ->client->sess.sessionTeam == TEAM_RED)
		flag_pw = PW_BLUEFLAG;
	else
		flag_pw = PW_REDFLAG;

	// flags
	if (targ->client->ps.powerups[flag_pw] &&
		targ->client->sess.sessionTeam != attacker->client->sess.sessionTeam)
		attacker->client->pers.teamState.lasthurtcarrier = level.time;

	// skulls
	if (targ->client->ps.generic1 &&
		targ->client->sess.sessionTeam != attacker->client->sess.sessionTeam)
		attacker->client->pers.teamState.lasthurtcarrier = level.time;
}


qboolean G_CallSpawn(gentity_t *ent);
void Team_StartOneFlagCapture(gentity_t *player, int team) {
	gentity_t *ent = NULL;
	gentity_t *newEnt = NULL;
	gitem_t		*item;

	while ((ent = G_Find(ent, FOFS(classname), "team_CTF_neutralflag")) != NULL) {
		G_FreeEntity(ent);
	}

	item = BG_FindItemForPowerup(PW_NEUTRALFLAG);
	player->client->ps.powerups[PW_NEUTRALFLAG] = 0;

	newEnt = G_Spawn(qtrue);
	if (team == TEAM_RED) {
		level.redCapturing = qtrue;
		newEnt->classname = "team_CTF_redflag";
		VectorCopy(level.redFlagOrigin, newEnt->s.origin);
	}
	else if (team == TEAM_BLUE) {
		level.blueCapturing = qtrue;
		newEnt->classname = "team_CTF_blueflag";
		VectorCopy(level.blueFlagOrigin, newEnt->s.origin);
	}

	if (!G_CallSpawn(newEnt))
		G_FreeEntity(newEnt);

	Team_SetFlagStatus(team, FLAG_ATBASE);
}

gentity_t *Team_ResetFlag( int team ) {
	char *c;
	gentity_t *ent, *rent = NULL;

	switch (team) {
	case TEAM_RED:
		c = "team_CTF_redflag";
		break;
	case TEAM_BLUE:
		c = "team_CTF_blueflag";
		break;
	case TEAM_FREE:
		c = "team_CTF_neutralflag";
		break;
	default:
		return NULL;
	}

	ent = NULL;
	while ((ent = G_Find (ent, FOFS(classname), c)) != NULL) {
		if (ent->flags & FL_DROPPED_ITEM)
			G_FreeEntity(ent);
		else {
			rent = ent;
			RespawnItem(ent);
		}
	}

	Team_SetFlagStatus( team, FLAG_ATBASE );

	return rent;
}

void Team_ResetFlags( void ) {
	if(level.gametype == GT_CTF)
	{
		Team_ResetFlag( TEAM_RED );
		Team_ResetFlag( TEAM_BLUE );
		if (g_neutralFlag.integer >= 4)
			Team_ResetFlag(TEAM_FREE);
	}
	else if (level.gametype == GT_CTY) {
		Team_ResetFlag(TEAM_RED);
		Team_ResetFlag(TEAM_BLUE);
	}
	else if ((level.gametype == GT_FFA || level.gametype == GT_TEAM) && g_neutralFlag.integer < 4)
		Team_ResetFlag( TEAM_FREE );
}

void Team_ReturnFlagSound( gentity_t *ent, int team ) {
	gentity_t	*te;

	if (ent == NULL) {
		//trap->Print ("Warning:  NULL passed to Team_ReturnFlagSound\n");
		return;
	}

	te = G_TempEntity( ent->s.pos.trBase, EV_GLOBAL_TEAM_SOUND );
	if( team == TEAM_BLUE ) {
		te->s.eventParm = GTS_RED_RETURN;
	}
	else {
		te->s.eventParm = GTS_BLUE_RETURN;
	}
	te->r.svFlags |= SVF_BROADCAST;
}

void Team_TakeFlagSound( gentity_t *ent, int team, int playerteam) {
	gentity_t	*te;

	if (ent == NULL) {
		trap->Print ("Warning:  NULL passed to Team_TakeFlagSound\n");
		return;
	}

	// only play sound when the flag was at the base
	// or not picked up the last 10 seconds
	switch(team) {
		case TEAM_RED:
			if( teamgame.blueStatus != FLAG_ATBASE ) {
				if (teamgame.blueTakenTime > level.time - 10000)
					return;
			}
			teamgame.blueTakenTime = level.time;
			break;

		case TEAM_BLUE:	// CTF
			if( teamgame.redStatus != FLAG_ATBASE ) {
				if (teamgame.redTakenTime > level.time - 10000)
					return;
			}
			teamgame.redTakenTime = level.time;
			break;
	}

	te = G_TempEntity( ent->s.pos.trBase, EV_GLOBAL_TEAM_SOUND );
	if ((level.gametype == GT_CTF || level.gametype == GT_TEAM) && g_neutralFlag.integer >= 4)
	{
		if (playerteam == TEAM_BLUE) {
			te->s.eventParm = GTS_BLUE_TAKEN;
		}
		else {
			te->s.eventParm = GTS_RED_TAKEN;
		}
	}
	else
	{
		if (team == TEAM_BLUE) {
			te->s.eventParm = GTS_RED_TAKEN;
		}
		else {
			te->s.eventParm = GTS_BLUE_TAKEN;
		}
	}
	te->r.svFlags |= SVF_BROADCAST;
}

void Team_CaptureFlagSound( gentity_t *ent, int team ) {
	gentity_t	*te;

	if (ent == NULL) {
		trap->Print ("Warning:  NULL passed to Team_CaptureFlagSound\n");
		return;
	}

	te = G_TempEntity( ent->s.pos.trBase, EV_GLOBAL_TEAM_SOUND );
	if( team == TEAM_BLUE ) {
		te->s.eventParm = GTS_BLUE_CAPTURE;
	}
	else {
		te->s.eventParm = GTS_RED_CAPTURE;
	}
	te->r.svFlags |= SVF_BROADCAST;
}

void Team_ReturnFlag( int team ) {
	Team_ReturnFlagSound(Team_ResetFlag(team), team);
	if( level.gametype != GT_CTF ) {
		//PrintMsg(NULL, "The flag has returned!\n" );
	}
	else { //flag should always have team in normal CTF
		//PrintMsg(NULL, "The %s flag has returned!\n", TeamName(team));
		PrintCTFMessage(-1, team, CTFMESSAGE_FLAG_RETURNED);
	}
}

void Team_FreeEntity( gentity_t *ent ) {
	if( ent->item->giTag == PW_REDFLAG ) {
		Team_ReturnFlag( TEAM_RED );
	}
	else if( ent->item->giTag == PW_BLUEFLAG ) {
		Team_ReturnFlag( TEAM_BLUE );
	}
	else if( ent->item->giTag == PW_NEUTRALFLAG ) {
		Team_ReturnFlag( TEAM_FREE );
	}
}

/*
==============
Team_DroppedFlagThink

Automatically set in Launch_Item if the item is one of the flags

Flags are unique in that if they are dropped, the base flag must be respawned when they time out
==============
*/
void Team_DroppedFlagThink(gentity_t *ent) {
	int		team = TEAM_FREE;

	if( ent->item->giTag == PW_REDFLAG ) {
		team = TEAM_RED;
	}
	else if( ent->item->giTag == PW_BLUEFLAG ) {
		team = TEAM_BLUE;
	}
	else if( ent->item->giTag == PW_NEUTRALFLAG ) {
		team = TEAM_FREE;
	}

	Team_ReturnFlagSound( Team_ResetFlag( team ), team );
	// Reset Flag will delete this entity
}
void G_AddSimpleStat(char *username, int type);
int Team_TouchOneFlagBase (gentity_t *ent, gentity_t *other, int team) {
	// the flag is at home base.  if the player has the enemy
	// flag, he's just won!
	int points = CTF_CAPTURE_BONUS;


	//We need to respawn the flag that we have on our back to midfield

	gclient_t	*cl = other->client;
	if (!cl->ps.powerups[PW_NEUTRALFLAG]) {
		return 0; // We don't have the flag
	}

	cl->ps.powerups[PW_NEUTRALFLAG] = 0;

	teamgame.last_flag_capture = level.time;
	teamgame.last_capture_team = team;

	// Increase the team's score
	AddTeamScore(ent->s.pos.trBase, other->client->sess.sessionTeam, 1, qtrue);
	//	Team_ForceGesture(other->client->sess.sessionTeam);
	//rww - don't really want to do this now. Mainly because performing a gesture disables your upper torso animations until it's done and you can't fire

	other->client->pers.teamState.captures++;
	other->client->rewardTime = level.time + REWARD_SPRITE_TIME;
	other->client->ps.persistant[PERS_CAPTURES]++;
	if (other->client->pers.userName[0])
	G_AddSimpleStat(other->client->pers.userName, 4);

	// other gets another 10 frag bonus
	if (g_fixCTFScores.integer)
		points = 15;

	AddScore(other, ent->r.currentOrigin, points);

	if (cl->pers.stats.startTimeFlag) {//JAPRO SHITTY FLAG TIMER
		const float time = (level.time - cl->pers.stats.startTimeFlag) / 1000.0f;
		//int average = floorf(cl->pers.stats.displacementFlag / time) + 0.5f;
		int average;
		if (cl->pers.stats.displacementFlagSamples)
			average = floorf(((cl->pers.stats.displacementFlag * sv_fps.value) / cl->pers.stats.displacementFlagSamples) + 0.5f);
		else
			average = cl->pers.stats.topSpeedFlag;

		trap->SendServerCommand(-1, va("print \"%s^5 has captured the %s^5 flag in ^3%.2f^5 seconds with max of ^3%i^5 ups and average ^3%i^5 ups (^3%i^5)\n\"", cl->pers.netname, team == 2 ? "^1red" : "^4blue", time, (int)floorf(cl->pers.stats.topSpeedFlag + 0.5f), average, points));
		cl->pers.stats.startTimeFlag = 0;
		cl->pers.stats.topSpeedFlag = 0;
		cl->pers.stats.displacementFlag = 0;
		cl->pers.stats.displacementFlagSamples = 0;
	}
	else if (g_fixCTFScores.integer) {
		trap->SendServerCommand(-1, va("print \"%s^5 has captured the %s^5 flag (+^315^5)\n\"", cl->pers.netname, team == 2 ? "^1red" : "^4blue"));
	}
	else
		PrintCTFMessage(other->s.number, team, CTFMESSAGE_PLAYER_CAPTURED_FLAG);

	#if _DEBUGCTFCRASH
	G_SecurityLogPrintf("Team_TouchOurFlag function reached point z, Enemy Flag is %i\n", enemy_flag);
	#endif

	Team_CaptureFlagSound(ent, team);

	Team_ResetFlags();

	CalculateRanks();

	return 0;
}


/*
==============
Team_DroppedFlagThink
==============
*/

// This is to account for situations when there are more players standing
// on flag stand and then flag gets returned. This leaded to bit random flag
// grabs/captures, improved version takes distance to the center of flag stand
// into consideration (closer player will get/capture the flag).
static vec3_t	minFlagRange = { 50, 36, 36 };
static vec3_t	maxFlagRange = { 44, 36, 36 };

int Team_TouchEnemyFlag( gentity_t *ent, gentity_t *other, int team );

#define _DEBUGCTFCRASH 0//Its been years so give up on trying to debug this if it even happens anymore
int Team_TouchOurFlag( gentity_t *ent, gentity_t *other, int team ) {
	int			i, num, j, enemyTeam;
	gentity_t	*player;
	gclient_t	*cl = other->client;
	int			enemy_flag;
	vec3_t		mins, maxs;
	int			touch[MAX_GENTITIES];
	gentity_t*	enemy;
	float		enemyDist, dist;

	if (cl->sess.sessionTeam == TEAM_RED) {
		enemy_flag = PW_BLUEFLAG;
	} else if (cl->sess.sessionTeam == TEAM_BLUE) {
		enemy_flag = PW_REDFLAG;
	} else { //rabbit
		enemy_flag = PW_NEUTRALFLAG;
	}

	if ( ent->flags & FL_DROPPED_ITEM ) {
		// hey, its not home.  return it by teleporting it back
		//PrintMsg( NULL, "%s" S_COLOR_WHITE " returned the %s flag!\n",
		//	cl->pers.netname, TeamName(team));
		if (other->client && g_fixCTFScores.integer) {
			float enemyDist, myDist, percent;
			int points;
			gentity_t *flag = NULL;
			char		*enemyflag, *myflag;
			if (team == TEAM_RED) {
				enemyflag = "team_CTF_blueflag";
				myflag = "team_CTF_redflag";

			}
			else if (team == TEAM_BLUE) {
				enemyflag = "team_CTF_redflag";
				myflag = "team_CTF_blueflag";
			}
			while ((flag = G_Find(flag, FOFS(classname), enemyflag)) != NULL) {
				if (flag->s.origin[0] || flag->s.origin[1] || flag->s.origin[2]) { //How better to tell if this flag is at home?
					enemyDist = Distance(flag->s.origin, ent->s.pos.trBase);
				}
				//Com_Printf("Found flag %s %.2f %.2f\n", classname, flag->s.origin[0], flag->s.origin[1]);
			}
			while ((flag = G_Find(flag, FOFS(classname), myflag)) != NULL) {
				if (flag->s.origin[0] || flag->s.origin[1] || flag->s.origin[2]) { //How better to tell if this flag is at home?
					myDist = Distance(flag->s.origin, ent->s.pos.trBase);
				}
				//Com_Printf("Found flag %s %.2f %.2f\n", classname, flag->s.origin[0], flag->s.origin[1]);
			}
			//Com_Printf("Touching enemy flag, it was %.0f away from our flag base.\n", points);
			//Com_Printf("Enemy dist %.2f, My dist %.2f\n", enemyDist, myDist);

			percent = (enemyDist + myDist) ? (myDist / ((enemyDist + myDist)) * 100) : 1;
			if (percent < 10) {
				trap->SendServerCommand(-1, va("print \"%s^5 has returned the %s^5 flag ^3<10^5 percent of the way to enemy base (+^30^5)\n\"", cl->pers.netname, team == 1 ? "^1red" : "^4blue"));
			}
			else {
				points = (int)((CTF_RECOVERY_BONUS * percent*0.01f)+0.5f);
				trap->SendServerCommand(-1, va("print \"%s^5 has returned the %s^5 flag ^3%.0f^5 percent of the way to enemy base (+^3%i^5)\n\"", cl->pers.netname, team == 1 ? "^1red" : "^4blue", percent, points));
			}

			AddScore(other, ent->r.currentOrigin, CTF_RECOVERY_BONUS);
		}
		else {
			PrintCTFMessage(other->s.number, team, CTFMESSAGE_PLAYER_RETURNED_FLAG);
			AddScore(other, ent->r.currentOrigin, CTF_RECOVERY_BONUS);
		}

		other->client->pers.teamState.flagrecovery++;
		if (other->client->pers.userName[0])
			G_AddSimpleStat(other->client->pers.userName, 5);


		other->client->pers.teamState.lastreturnedflag = level.time;
		//ResetFlag will remove this entity!  We must return zero
		Team_ReturnFlagSound(Team_ResetFlag(team), team); //is this crashing wtf
		return 0;
	}

	// the flag is at home base.  if the player has the enemy
	// flag, he's just won!
	if (!cl->ps.powerups[enemy_flag]) {
		return 0; // We don't have the flag
	}

	// fix: captures after timelimit hit could
	// cause game ending with tied score
	if (level.intermissionQueued) {
		return 0;
	}

#if _DEBUGCTFCRASH
	G_SecurityLogPrintf("Team_TouchOurFlag function reached point a, Enemy Flag is %i\n", enemy_flag);
#endif

	// check for enemy closer to grab the flag
	VectorSubtract( ent->s.pos.trBase, minFlagRange, mins );
	VectorAdd( ent->s.pos.trBase, maxFlagRange, maxs );

	num = trap->EntitiesInBox( mins, maxs, touch, MAX_GENTITIES );

	dist = Distance( ent->s.pos.trBase, other->client->ps.origin );

	if (other->client->sess.sessionTeam == TEAM_RED)
		enemyTeam = TEAM_BLUE;
	else if (other->client->sess.sessionTeam == TEAM_BLUE)
		enemyTeam = TEAM_RED;
	else 
		enemyTeam = TEAM_FREE; //racemode ctf crashfix?

#if _DEBUGCTFCRASH
	G_SecurityLogPrintf("Team_TouchOurFlag function reached point b, Enemy Flag is %i\n", enemy_flag);
#endif

	for (j = 0; j < num; j++) {
		enemy = (g_entities + touch[j]);

		if (!enemy || !enemy->inuse || !enemy->client)
			continue;

		if (enemy->client->pers.connected != CON_CONNECTED)
			continue;

		//check if its alive
		if (enemy->health < 1)
			continue;		// dead people can't pickup

		//ignore specs
		if (enemy->client->sess.sessionTeam == TEAM_SPECTATOR)
			continue;

		//check if this is enemy
		if ((enemy->client->sess.sessionTeam != TEAM_RED && enemy->client->sess.sessionTeam != TEAM_BLUE) ||
			enemy->client->sess.sessionTeam != enemyTeam){
			continue;
		}

		//check if enemy is closer to our flag than us
		enemyDist = Distance(ent->s.pos.trBase, enemy->client->ps.origin);
		if (enemyDist < dist) {
			// possible recursion is hidden in this, but
			// infinite recursion wont happen, because we cant
			// have a < b and b < a at the same time
			return Team_TouchEnemyFlag( ent, enemy, team );
		}
	}

#if _DEBUGCTFCRASH
	G_SecurityLogPrintf("Team_TouchOurFlag function reached point c, Enemy Flag is %i\n", enemy_flag);
#endif

	//PrintMsg( NULL, "%s" S_COLOR_WHITE " captured the %s flag!\n", cl->pers.netname, TeamName(OtherTeam(team)));

	cl->ps.powerups[enemy_flag] = 0;

	teamgame.last_flag_capture = level.time;
	teamgame.last_capture_team = team;

	// Increase the team's score
	AddTeamScore(ent->s.pos.trBase, other->client->sess.sessionTeam, 1, qtrue);

	other->client->pers.teamState.captures++;
	other->client->rewardTime = level.time + REWARD_SPRITE_TIME;
	other->client->ps.persistant[PERS_CAPTURES]++;
	if (other->client->pers.userName[0])
			G_AddSimpleStat(other->client->pers.userName, 4);

	// other gets another 10 frag bonus
	if (g_fixCTFScores.integer)
		AddScore(other, ent->r.currentOrigin, 15);
	else
		AddScore(other, ent->r.currentOrigin, CTF_CAPTURE_BONUS);

	if (cl->pers.stats.startTimeFlag) {//JAPRO SHITTY FLAG TIMER
		const float time = (level.time - cl->pers.stats.startTimeFlag) / 1000.0f;
		//int average = floorf(cl->pers.stats.displacementFlag / time) + 0.5f;
		int average;
		if (cl->pers.stats.displacementFlagSamples)
			average = floorf(((cl->pers.stats.displacementFlag * sv_fps.value) / cl->pers.stats.displacementFlagSamples) + 0.5f);
		else
			average = cl->pers.stats.topSpeedFlag;

		trap->SendServerCommand( -1, va("print \"%s^5 has captured the %s^5 flag in ^3%.2f^5 seconds with max of ^3%i^5 ups and average ^3%i^5 ups (+^3%i^5)\n\"", cl->pers.netname, team == 2 ? "^1red" : "^4blue", time, (int)floorf(cl->pers.stats.topSpeedFlag + 0.5f), average, g_fixCTFScores.integer ? 15 : CTF_CAPTURE_BONUS));
		cl->pers.stats.startTimeFlag = 0;
		cl->pers.stats.topSpeedFlag = 0;
		cl->pers.stats.displacementFlag = 0;
		cl->pers.stats.displacementFlagSamples = 0;
	}
	else if (g_fixCTFScores.integer) {
		trap->SendServerCommand(-1, va("print \"%s^5 has captured the %s^5 flag (+^315^5)\n\"", cl->pers.netname, team == 2 ? "^1red" : "^4blue"));
	}
	else
		PrintCTFMessage(other->s.number, team, CTFMESSAGE_PLAYER_CAPTURED_FLAG);  //How show score for capping midfielded flag?

#if _DEBUGCTFCRASH
	G_SecurityLogPrintf("Team_TouchOurFlag function reached point d, Enemy Flag is %i\n", enemy_flag);
#endif

	Team_CaptureFlagSound( ent, team );

	// Ok, let's do the player loop, hand out the bonuses
	for (i = 0; i < sv_maxclients.integer; i++) {
		player = &g_entities[i];
		if (!player->inuse || player == other)
			continue;

		if (player->client->sess.sessionTeam !=
			cl->sess.sessionTeam) {
			player->client->pers.teamState.lasthurtcarrier = -5;
		} else if (player->client->sess.sessionTeam ==
			cl->sess.sessionTeam) {
			if (!g_fixCTFScores.integer)
				AddScore(player, ent->r.currentOrigin, CTF_TEAM_BONUS);
			// award extra points for capture assists
			if (player->client->pers.teamState.lastreturnedflag +
				CTF_RETURN_FLAG_ASSIST_TIMEOUT > level.time) {
				if (g_fixCTFScores.integer)
					AddScore(player, ent->r.currentOrigin, 3);
				else
					AddScore (player, ent->r.currentOrigin, CTF_RETURN_FLAG_ASSIST_BONUS);
				other->client->pers.teamState.assists++;

				player->client->ps.persistant[PERS_ASSIST_COUNT]++;
				player->client->rewardTime = level.time + REWARD_SPRITE_TIME;

			}
			if (player->client->pers.teamState.lastfraggedcarrier +
				CTF_FRAG_CARRIER_ASSIST_TIMEOUT > level.time) {
				if (g_fixCTFScores.integer)
					AddScore(player, ent->r.currentOrigin, 3);
				else
					AddScore(player, ent->r.currentOrigin, CTF_FRAG_CARRIER_ASSIST_BONUS);
				other->client->pers.teamState.assists++;
				player->client->ps.persistant[PERS_ASSIST_COUNT]++;
				player->client->rewardTime = level.time + REWARD_SPRITE_TIME;
			}
		}
	}

#if _DEBUGCTFCRASH
	G_SecurityLogPrintf("Team_TouchOurFlag function reached point e, Enemy Flag is %i\n", enemy_flag);
#endif

	Team_ResetFlags();

	CalculateRanks();

#if _DEBUGCTFCRASH
	G_SecurityLogPrintf("Team_TouchOurFlag function exited at end, Enemy Flag is %i\n", enemy_flag);
#endif

	return 0; // Do not respawn this automatically
}

int Team_TouchEnemyFlag( gentity_t *ent, gentity_t *other, int team ) {
	gclient_t *cl = other->client;
	vec3_t		mins, maxs;
	int			num, j, ourFlag;
	int			touch[MAX_GENTITIES];
	gentity_t*	enemy;
	float		enemyDist, dist;
	int points = 0;

#if _DEBUGCTFCRASH
	G_SecurityLogPrintf("Team_TouchEnemyFlag called \n");
#endif

	VectorSubtract( ent->s.pos.trBase, minFlagRange, mins );
	VectorAdd( ent->s.pos.trBase, maxFlagRange, maxs );

	num = trap->EntitiesInBox( mins, maxs, touch, MAX_GENTITIES );

	dist = Distance(ent->s.pos.trBase, other->client->ps.origin);

	if (other->client->sess.sessionTeam == TEAM_RED)
		ourFlag = PW_REDFLAG;
	else if (other->client->sess.sessionTeam == TEAM_BLUE)
		ourFlag = PW_BLUEFLAG;
	else 
		ourFlag = PW_NEUTRALFLAG;//rabbit

	for(j = 0; j < num; ++j){
		enemy = (g_entities + touch[j]);

		if (!enemy || !enemy->inuse || !enemy->client){
			continue;
		}

		//ignore specs
		if (enemy->client->sess.sessionTeam == TEAM_SPECTATOR)
			continue;

		//check if its alive
		if (enemy->health < 1)
			continue;		// dead people can't pick up items

		//lets check if he has our flag
		if (!enemy->client->ps.powerups[ourFlag])
			continue;

		//check if enemy is closer to our flag than us
		enemyDist = Distance(ent->s.pos.trBase,enemy->client->ps.origin);
		if (enemyDist < dist){
			// possible recursion is hidden in this, but
			// infinite recursion wont happen, because we cant
			// have a < b and b < a at the same time
#if _DEBUGCTFCRASH
	G_SecurityLogPrintf("Team_TouchEnemyFlag returned at point 1, team %i, ourflag %i\n", team, ourFlag);
#endif
			return Team_TouchOurFlag( ent, enemy, team );
		}
	}

	//PrintMsg (NULL, "%s" S_COLOR_WHITE " got the %s flag!\n",
	//	other->client->pers.netname, TeamName(team));
	if (level.gametype != GT_CTF) { //changed this from team == team_free
		trap->SendServerCommand( -1, va("print \"%s^7 is now the rabbit!\n\"", other->client->pers.netname ));
		if (g_neutralFlag.integer == 2) {
			other->client->ps.stats[STAT_WEAPONS] = (1 << WP_DISRUPTOR);
			other->client->ps.ammo[AMMO_POWERCELL] = 300;
		}
		else if (g_neutralFlag.integer == 3) {
			other->client->timeResidualBig = 0; //Reset this so we dont get any points until 5s after picking up the flag. (5000 - 0) = 5s
		}
	}
	else {
		if (g_neutralFlag.integer == 6) {
			if (team == TEAM_RED) {
				level.redCapturing = qfalse;
			}
			else if (team == TEAM_BLUE) {
				level.blueCapturing = qfalse;
			}
		}

		if (g_fixCTFScores.integer) {
			float speed = VectorLength(other->client->ps.velocity);
			if (speed > 2000)
				points = 2;
			else if (speed > 1000)
				points = 1;

			if (team == TEAM_RED) {
				if (teamgame.blueStatus == FLAG_ATBASE && teamgame.redStatus == FLAG_ATBASE) {
					if (g_neutralFlag.integer == 6) {
						points += 3;
						trap->SendServerCommand(-1, va("print \"%s ^5denied the flag at ^1red^5 base at ^3%.0f^5 ups (+^3%i^5)\n\"", other->client->pers.netname, speed, points));
					}
					else 
						trap->SendServerCommand(-1, va("print \"%s ^5grabbed the ^1red^5 flag at ^3%.0f^5 ups (+^3%i^5)\n\"", other->client->pers.netname, speed, points));
				}
				else if (teamgame.blueStatus != FLAG_ATBASE) {
					points += 1;
					trap->SendServerCommand(-1, va("print \"%s ^5e-grabbed the ^1red^5 flag at ^3%.0f^5 ups (+^3%i^5)\n\"", other->client->pers.netname, speed, points));
				}
				else
					trap->SendServerCommand(-1, va("print \"%s ^5grabbed the ^1red^5 flag\n\"", other->client->pers.netname));
			}
			else if (team == TEAM_BLUE) {
				if (teamgame.redStatus == FLAG_ATBASE && teamgame.blueStatus == FLAG_ATBASE) {
					if (g_neutralFlag.integer == 6) {
						points += 3;
						trap->SendServerCommand(-1, va("print \"%s ^5denied the flag at ^4blue^5 base at ^3%.0f^5 ups (+^3%i^5)\n\"", other->client->pers.netname, speed, points));
					}
					else
						trap->SendServerCommand(-1, va("print \"%s ^5grabbed the ^4blue^5 flag at ^3%.0f^5 ups (+^3%i^5)\n\"", other->client->pers.netname, speed, points));
				}
				else if (teamgame.redStatus != FLAG_ATBASE) {
					points += 1;
					trap->SendServerCommand(-1, va("print \"%s ^5e-grabbed the ^4blue^5 flag at ^3%.0f^5 ups (+^3%i^5)\n\"", other->client->pers.netname, speed, points));
				}
				else
					trap->SendServerCommand(-1, va("print \"%s ^5grabbed the ^4blue^5 flag\n\"", other->client->pers.netname));
			}
			else {
				if (teamgame.flagStatus == FLAG_ATBASE) {
					points += 1;
					trap->SendServerCommand(-1, va("print \"%s ^5grabbed the flag at ^3%.0f^5 ups (+^3%i^5)\n\"", other->client->pers.netname, speed, points));
				}
				else if (teamgame.flagStatus != FLAG_ATBASE) {
					trap->SendServerCommand(-1, va("print \"%s ^5fielded the flag at ^3%.0f^5 ups (+^3%i^5)\n\"", other->client->pers.netname, speed, points));
				}
				else
					trap->SendServerCommand(-1, va("print \"%s ^5grabbed the flag\n\"", other->client->pers.netname));
			}
		}
		else {
			PrintCTFMessage(other->s.number, team, CTFMESSAGE_PLAYER_GOT_FLAG);
		}
	}

	if (level.gametype == GT_CTF && g_neutralFlag.integer == 6) {
		cl->ps.powerups[PW_NEUTRALFLAG] = INT_MAX; // flags never expire
	}
	else {
		if (team == TEAM_RED)
			cl->ps.powerups[PW_REDFLAG] = INT_MAX; // flags never expire
		else if (team == TEAM_BLUE)
			cl->ps.powerups[PW_BLUEFLAG] = INT_MAX; // flags never expire
		else//Rabbit
			cl->ps.powerups[PW_NEUTRALFLAG] = INT_MAX; // flags never expire
	}

	if ((team == TEAM_RED && teamgame.redStatus == FLAG_ATBASE) || (team == TEAM_BLUE && teamgame.blueStatus == FLAG_ATBASE)) {//JAPRO SHITTY FLAG TIMER
		cl->pers.stats.startTimeFlag = level.time;
		cl->pers.stats.topSpeedFlag = 0;
		cl->pers.stats.displacementFlag = 0;
	}
	else if (level.gametype == GT_CTF && g_neutralFlag.integer >= 4 && !(ent->r.contents & CONTENTS_CORPSE))
	{
		cl->pers.stats.startTimeFlag = level.time;
		cl->pers.stats.topSpeedFlag = 0;
		cl->pers.stats.displacementFlag = 0;
	}
	else
		cl->pers.stats.startTimeFlag = 0;

	if (g_fixCTFScores.integer) {
		if (points) {
			AddScore(other, ent->r.currentOrigin, points);
		}
	}
	else if (!g_allowFlagThrow.integer && (g_neutralFlag.integer != 2)) {
		AddScore(other, ent->r.currentOrigin, CTF_FLAG_BONUS);
	}

	// Increase the team's score
	//AddTeamScore(ent->s.pos.trBase, other->client->sess.sessionTeam, 1, qtrue); //wtf why?

	Team_SetFlagStatus( team, FLAG_TAKEN );

	cl->pers.teamState.flagsince = level.time;
	Team_TakeFlagSound( ent, team, cl->sess.sessionTeam);

#if _DEBUGCTFCRASH
	G_SecurityLogPrintf("Team_TouchEnemyFlag returned at emd, team %i, ourflag %i\n", team, ourFlag);
#endif

	return -1; // Do not respawn this automatically, but do delete it if it was FL_DROPPED
}

int Pickup_Team( gentity_t *ent, gentity_t *other ) {
	int team;
	gclient_t *cl = other->client;

	// figure out what team this flag is
	if( strcmp(ent->classname, "team_CTF_redflag") == 0 ) {
		team = TEAM_RED;
	}
	else if( strcmp(ent->classname, "team_CTF_blueflag") == 0 ) {
		team = TEAM_BLUE;
	}
	else if( strcmp(ent->classname, "team_CTF_neutralflag") == 0  ) {
		team = TEAM_FREE;
	}
	else {
//		PrintMsg ( other, "Don't know what team the flag is on.\n");
		return 0;
	}
	// GT_CTF
	if( team == cl->sess.sessionTeam) {
		if ((level.gametype == GT_FFA || level.gametype == GT_TEAM) && g_neutralFlag.integer < 4) {
			return Team_TouchEnemyFlag( ent, other, team );
		}
		else if ((level.gametype == GT_CTF) && g_neutralFlag.integer >= 4) {
			return Team_TouchEnemyFlag(ent, other, team);
		}
		else {
			return Team_TouchOurFlag( ent, other, team );
		}
	}
	return Team_TouchEnemyFlag( ent, other, team );
}

/*
===========
Team_GetLocation

Report a location for the player. Uses placed nearby target_location entities
============
*/
locationData_t *Team_GetLocation(gentity_t *ent)
{
	locationData_t	*loc, *best;
	float			bestlen, len;
	vec3_t			origin;
	int				i;

	best = NULL;
	bestlen = 3*8192.0*8192.0;

	VectorCopy( ent->r.currentOrigin, origin );

	for ( i=0; i<level.locations.num; i++ ) {
		loc = &level.locations.data[i];
		len = ( origin[0] - loc->origin[0] ) * ( origin[0] - loc->origin[0] )
			+ ( origin[1] - loc->origin[1] ) * ( origin[1] - loc->origin[1] )
			+ ( origin[2] - loc->origin[2] ) * ( origin[2] - loc->origin[2] );

		if ( len > bestlen ) {
			continue;
		}

		if ( !trap->InPVS( origin, loc->origin ) ) {
			continue;
		}

		bestlen = len;
		best = loc;
	}

	return best;
}


/*
===========
Team_GetLocation

Report a location for the player. Uses placed nearby target_location entities
============
*/
qboolean Team_GetLocationMsg(gentity_t *ent, char *loc, int loclen)
{
	locationData_t *best;

	if (ent->client && ent->client->sess.sessionTeam == TEAM_SPECTATOR) //Dont do loc text if we are in spec..
		return qfalse;

	best = Team_GetLocation( ent );

	if (!best)
		return qfalse;

	if (best->count) {
		if (best->count < 0)
			best->count = 0;
		if (best->count > 7)
			best->count = 7;
		Com_sprintf(loc, loclen, "%c%c%s" S_COLOR_WHITE, Q_COLOR_ESCAPE, best->count + '0', best->message );
	} else
		Com_sprintf(loc, loclen, "%s", best->message);

	return qtrue;
}


/*---------------------------------------------------------------------------*/

/*
================
SelectRandomTeamSpawnPoint

go to a random point that doesn't telefrag
================
*/
#define	MAX_TEAM_SPAWN_POINTS	32
gentity_t *SelectRandomTeamSpawnPoint( int teamstate, team_t team, int siegeClass ) {
	gentity_t	*spot;
	int			count;
	int			selection;
	gentity_t	*spots[MAX_TEAM_SPAWN_POINTS];
	const char	*classname;
	qboolean	mustBeEnabled = qfalse;

	if (level.gametype == GT_SIEGE)
	{
		if (team == SIEGETEAM_TEAM1)
		{
			classname = "info_player_siegeteam1";
		}
		else
		{
			classname = "info_player_siegeteam2";
		}

		mustBeEnabled = qtrue; //siege spawn points need to be "enabled" to be used (because multiple spawnpoint sets can be placed at once)
	}
	else
	{
		if (teamstate == TEAM_BEGIN) {
			if (team == TEAM_RED)
				classname = "team_CTF_redplayer";
			else if (team == TEAM_BLUE)
				classname = "team_CTF_blueplayer";
			else
				return NULL;
		} else {
			if (team == TEAM_RED)
				classname = "team_CTF_redspawn";
			else if (team == TEAM_BLUE)
				classname = "team_CTF_bluespawn";
			else
				return NULL;
		}
	}
	count = 0;

	spot = NULL;

	while ((spot = G_Find (spot, FOFS(classname), classname)) != NULL) {
		if ( SpotWouldTelefrag( spot ) ) {
			continue;
		}

		if (mustBeEnabled && !spot->genericValue1)
		{ //siege point that's not enabled, can't use it
			continue;
		}

		//Seperate spawning for 1flag and 2flag ctf?
		if ((spot->spawnflags & 2) && level.gametype == GT_CTF && g_neutralFlag.integer >= 4) //eh?
			continue;
		if ((spot->spawnflags & 1) && level.gametype == GT_CTF && !g_neutralFlag.integer)
			continue;
		//

		spots[ count ] = spot;
		if (++count == MAX_TEAM_SPAWN_POINTS)
			break;
	}

	if ( !count ) {	// no spots that won't telefrag
		return G_Find( NULL, FOFS(classname), classname);
	}

	if (level.gametype == GT_SIEGE && siegeClass >= 0 &&
		bgSiegeClasses[siegeClass].name[0])
	{ //out of the spots found, see if any have an idealclass to match our class name
		gentity_t *classSpots[MAX_TEAM_SPAWN_POINTS];
		int classCount = 0;
		int i = 0;

        while (i < count)
		{
			if (spots[i] && spots[i]->idealclass && spots[i]->idealclass[0] &&
				!Q_stricmp(spots[i]->idealclass, bgSiegeClasses[siegeClass].name))
			{ //this spot's idealclass matches the class name
                classSpots[classCount] = spots[i];
				classCount++;
			}
			i++;
		}

		if (classCount > 0)
		{ //found at least one
			selection = rand() % classCount;
			return classSpots[ selection ];
		}
	}

	selection = rand() % count;
	return spots[ selection ];
}


/*
===========
SelectCTFSpawnPoint

============
*/
gentity_t *SelectCTFSpawnPoint ( team_t team, int teamstate, vec3_t origin, vec3_t angles, qboolean isbot, gclient_t *client) {
	gentity_t	*spot;
	qboolean caprouteOverride = qfalse;

	spot = SelectRandomTeamSpawnPoint ( teamstate, team, -1 );

	if (!spot) {
		return SelectSpawnPoint( vec3_origin, origin, angles, team, isbot );
	}

	if (isbot && g_tribesMode.integer) { //Need to do a better check for if we want to make them a capper bot, pass in isCapperBot?  client model? netname?
		const int MAX_ROUTES_PER_TEAM = 6;
		int i;
		if (team == TEAM_RED) {
			int numRedRoutes = 0;
			for (i = 0; i < MAX_ROUTES_PER_TEAM; i++) {
				if (redRouteList[i].length > 0)
					numRedRoutes++;
				else break;
			}
			if (numRedRoutes) {
				int randomPick = Q_irand(1, numRedRoutes);//to array #
				origin[0] = (int)(redRouteList[randomPick-1].pos[0][0]);
				origin[1] = (int)(redRouteList[randomPick-1].pos[0][1]);
				origin[2] = (int)(redRouteList[randomPick-1].pos[0][2]);
				caprouteOverride = qtrue;
				if (client)
					client->pers.activeCapRoute = randomPick;
			}


		}
		else if (team == TEAM_BLUE) {
			int numBlueRoutes = 0;
			for (i = 0; i < MAX_ROUTES_PER_TEAM; i++) {
				if (blueRouteList[i].length > 0)
					numBlueRoutes++;
				else break;
			}
			if (numBlueRoutes) {
				int randomPick = Q_irand(1, numBlueRoutes);//to array #
				origin[0] = (int)(blueRouteList[randomPick-1].pos[0][0]);
				origin[1] = (int)(blueRouteList[randomPick-1].pos[0][1]);
				origin[2] = (int)(blueRouteList[randomPick-1].pos[0][2]);
				caprouteOverride = qtrue;
				if (client)
					client->pers.activeCapRoute = randomPick;
			}
		}
	}
	
	if (!caprouteOverride) {
		VectorCopy(spot->s.origin, origin);
		origin[2] += 9;
		VectorCopy(spot->s.angles, angles);
	}

	return spot;
}

/*
===========
SelectSiegeSpawnPoint

============
*/
gentity_t *SelectSiegeSpawnPoint ( int siegeClass, team_t team, int teamstate, vec3_t origin, vec3_t angles, qboolean isbot ) {
	gentity_t	*spot;

	spot = SelectRandomTeamSpawnPoint ( teamstate, team, siegeClass );

	if (!spot) {
		return SelectSpawnPoint( vec3_origin, origin, angles, team, isbot );
	}

	VectorCopy (spot->s.origin, origin);
	origin[2] += 9;
	VectorCopy (spot->s.angles, angles);

	return spot;
}

/*---------------------------------------------------------------------------*/

static int QDECL SortClients( const void *a, const void *b ) {
	return *(int *)a - *(int *)b;
}


/*
==================
TeamplayLocationsMessage

Format:
	clientNum location health armor weapon powerups

==================
*/
void TeamplayInfoMessage( gentity_t *ent ) {
	char		entry[1024];
	char		string[8192];
	int			stringlength;
	int			i, j;
	gentity_t	*player;
	int			cnt;
	int			h, a;
	int			clients[TEAM_MAXOVERLAY];
	int			team;

	if ( ! ent->client->pers.teamInfo )
		return;

	// send team info to spectator for team of followed client
	if (ent->client->sess.sessionTeam == TEAM_SPECTATOR) {
		if ( ent->client->sess.spectatorState != SPECTATOR_FOLLOW
			|| ent->client->sess.spectatorClient < 0 ) {
				return;
		}
		team = g_entities[ ent->client->sess.spectatorClient ].client->sess.sessionTeam;
	} else {
		team = ent->client->sess.sessionTeam;
	}

	if (team != TEAM_RED && team != TEAM_BLUE) {
		return;
	}

	// figure out what client should be on the display
	// we are limited to 8, but we want to use the top eight players
	// but in client order (so they don't keep changing position on the overlay)
	for (i = 0, cnt = 0; i < sv_maxclients.integer && cnt < TEAM_MAXOVERLAY; i++) {
		player = g_entities + level.sortedClients[i];
		if (player->inuse && player->client->sess.sessionTeam == team ) {
			clients[cnt++] = level.sortedClients[i];
		}
	}

	// We have the top eight players, sort them by clientNum
	qsort( clients, cnt, sizeof( clients[0] ), SortClients );

	// send the latest information on all clients
	string[0] = 0;
	stringlength = 0;

	for (i = 0, cnt = 0; i < sv_maxclients.integer && cnt < TEAM_MAXOVERLAY; i++) {
		player = g_entities + i;
		if (player->inuse && player->client->sess.sessionTeam == team ) {

			if ( player->client->tempSpectate >= level.time ) {
				h = a = 0;

				Com_sprintf( entry, sizeof(entry),
					" %i %i %i %i %i %i",
					i, 0, h, a, 0, 0 );
			}
			else {
				h = player->client->ps.stats[STAT_HEALTH];
				a = player->client->ps.stats[STAT_ARMOR] * 100 + player->client->ps.fd.forcePower;
				if ( h < 0 ) h = 0;
				if ( a < 0 ) a = 0;

				Com_sprintf( entry, sizeof(entry),
					" %i %i %i %i %i %i",
					i, player->client->pers.teamState.location, h, a,
					player->client->ps.weapon, player->s.powerups );
			}
			j = strlen(entry);
			if (stringlength + j >= sizeof(string))
				break;
			strcpy (string + stringlength, entry);
			stringlength += j;
			cnt++;
		}
	}

	trap->SendServerCommand( ent-g_entities, va("tinfo %i %s", cnt, string) );
}

void CheckTeamStatus(void) {
	int i;
	locationData_t *loc;
	gentity_t *ent;

	//OSP: pause
	if ( level.pause.state != PAUSE_NONE ) //doesnt affect racers since thats team_free i guess
		return;

	if (level.time - level.lastTeamLocationTime > TEAM_LOCATION_UPDATE_TIME) {

		level.lastTeamLocationTime = level.time;

		for (i = 0; i < sv_maxclients.integer; i++) {
			ent = g_entities + i;

			if ( !ent->client )
			{
				continue;
			}

			if ( ent->client->pers.connected != CON_CONNECTED ) {
				continue;
			}

			if (ent->inuse && (ent->client->sess.sessionTeam == TEAM_RED ||	ent->client->sess.sessionTeam == TEAM_BLUE)) {
				loc = Team_GetLocation( ent );
				if (loc)
					ent->client->pers.teamState.location = loc->cs_index;
				else
					ent->client->pers.teamState.location = 0;
			}
		}

		for (i = 0; i < sv_maxclients.integer; i++) {
			ent = g_entities + i;

			if ( !ent->client ) // uhm
				continue;

			if ( ent->client->pers.connected != CON_CONNECTED ) {
				continue;
			}

			if (ent->inuse) {
				TeamplayInfoMessage( ent );
			}
		}
	}
}

/*-----------------------------------------------------------------*/

/*QUAKED team_CTF_redplayer (1 0 0) (-16 -16 -16) (16 16 32)
Only in CTF games.  Red players spawn here at game start.
*/
void SP_team_CTF_redplayer( gentity_t *ent ) {
}


/*QUAKED team_CTF_blueplayer (0 0 1) (-16 -16 -16) (16 16 32)
Only in CTF games.  Blue players spawn here at game start.
*/
void SP_team_CTF_blueplayer( gentity_t *ent ) {
}


/*QUAKED team_CTF_redspawn (1 0 0) (-16 -16 -24) (16 16 32)
potential spawning position for red team in CTF games.
Targets will be fired when someone spawns in on them.
*/
void SP_team_CTF_redspawn(gentity_t *ent) {
}

/*QUAKED team_CTF_bluespawn (0 0 1) (-16 -16 -24) (16 16 32)
potential spawning position for blue team in CTF games.
Targets will be fired when someone spawns in on them.
*/
void SP_team_CTF_bluespawn(gentity_t *ent) {
}



