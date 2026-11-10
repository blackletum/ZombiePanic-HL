/***
*
*	Copyright (c) 1996-2001, Valve LLC. All rights reserved.
*	
*	This product contains software technology licensed from Id 
*	Software, Inc. ("Id Technology").  Id Technology (c) 1996 Id Software, Inc. 
*	All Rights Reserved.
*
*   Use, distribution, and modification of this source code and/or resulting
*   object code is restricted to non-commercial enhancements to products from
*   Valve LLC.  All other use, distribution, or modification is prohibited
*   without written permission from Valve LLC.
*
****/

//	-------------------------------------------
//
//	maprules.cpp
//
//	This module contains entities for implementing/changing game
//	rules dynamically within each map (.BSP)
//
//	-------------------------------------------

#include "extdll.h"
#include "eiface.h"
#include "util.h"
#include "gamerules.h"
#include "maprules.h"
#include "cbase.h"
#include "player.h"
#ifdef SCRIPT_SYSTEM
#include "core.h"
#endif
#include "zp/zp_gamerules.h"

// for std::vector random_shuffle
#include <iterator>
#include <random>
#include <algorithm>

class CRuleEntity : public CBaseEntity
{
public:
	void Spawn(void);
	void KeyValue(KeyValueData *pkvd);
	virtual int Save(CSave &save);
	virtual int Restore(CRestore &restore);
	static TYPEDESCRIPTION m_SaveData[];

	void SetMaster(int iszMaster) { m_iszMaster = iszMaster; }

protected:
	BOOL CanFireForActivator(CBaseEntity *pActivator);

private:
	string_t m_iszMaster;
};

TYPEDESCRIPTION CRuleEntity::m_SaveData[] = {
	DEFINE_FIELD(CRuleEntity, m_iszMaster, FIELD_STRING),
};

IMPLEMENT_SAVERESTORE(CRuleEntity, CBaseEntity);

void CRuleEntity::Spawn(void)
{
	CBaseEntity::Spawn();
	pev->solid = SOLID_NOT;
	pev->movetype = MOVETYPE_NONE;
	pev->effects = EF_NODRAW;
}

void CRuleEntity::KeyValue(KeyValueData *pkvd)
{
	if (FStrEq(pkvd->szKeyName, "master"))
	{
		SetMaster(ALLOC_STRING(pkvd->szValue));
		pkvd->fHandled = TRUE;
	}
	else
		CBaseEntity::KeyValue(pkvd);
}

BOOL CRuleEntity::CanFireForActivator(CBaseEntity *pActivator)
{
	if (m_iszMaster)
	{
		if (UTIL_IsMasterTriggered(m_iszMaster, pActivator))
			return TRUE;
		else
			return FALSE;
	}

	return TRUE;
}

//
// CRulePointEntity -- base class for all rule "point" entities (not brushes)
//
class CRulePointEntity : public CRuleEntity
{
public:
	void Spawn(void);
};

void CRulePointEntity::Spawn(void)
{
	CRuleEntity::Spawn();
	pev->frame = 0;
	pev->model = 0;
}

//
// CRuleBrushEntity -- base class for all rule "brush" entities (not brushes)
// Default behavior is to set up like a trigger, invisible, but keep the model for volume testing
//
class CRuleBrushEntity : public CRuleEntity
{
public:
	void Spawn(void);

private:
};

void CRuleBrushEntity::Spawn(void)
{
	SET_MODEL(edict(), STRING(pev->model));
	CRuleEntity::Spawn();
}

// CGameScore / game_score	-- award points to player / team
//	Points +/- total
//	Flag: Allow negative scores					SF_SCORE_NEGATIVE
//	Flag: Award points to team in teamplay		SF_SCORE_TEAM

#define SF_SCORE_NEGATIVE 0x0001
#define SF_SCORE_TEAM     0x0002

class CGameScore : public CRulePointEntity
{
public:
	void Spawn(void);
	void Use(CBaseEntity *pActivator, CBaseEntity *pCaller, USE_TYPE useType, float value);
	void KeyValue(KeyValueData *pkvd);

	inline int Points(void) { return pev->frags; }
	inline BOOL AllowNegativeScore(void) { return pev->spawnflags & SF_SCORE_NEGATIVE; }
	inline BOOL AwardToTeam(void) { return pev->spawnflags & SF_SCORE_TEAM; }

	inline void SetPoints(int points) { pev->frags = points; }

private:
};

LINK_ENTITY_TO_CLASS(game_score, CGameScore);

void CGameScore::Spawn(void)
{
	CRulePointEntity::Spawn();
}

void CGameScore::KeyValue(KeyValueData *pkvd)
{
	if (FStrEq(pkvd->szKeyName, "points"))
	{
		SetPoints(atoi(pkvd->szValue));
		pkvd->fHandled = TRUE;
	}
	else
		CRulePointEntity::KeyValue(pkvd);
}

void CGameScore::Use(CBaseEntity *pActivator, CBaseEntity *pCaller, USE_TYPE useType, float value)
{
	if (!CanFireForActivator(pActivator))
		return;

	// Only players can use this
	if (pActivator->IsPlayer())
	{
		if (AwardToTeam())
		{
			pActivator->AddPointsToTeam(Points(), AllowNegativeScore());
		}
		else
		{
			pActivator->AddPoints(Points(), AllowNegativeScore());
		}
	}
}

// CGameEnd / game_end	-- Ends the game in MP

class CGameEnd : public CRulePointEntity
{
public:
	void Use(CBaseEntity *pActivator, CBaseEntity *pCaller, USE_TYPE useType, float value);

private:
};

LINK_ENTITY_TO_CLASS(game_end, CGameEnd);

void CGameEnd::Use(CBaseEntity *pActivator, CBaseEntity *pCaller, USE_TYPE useType, float value)
{
	if (!CanFireForActivator(pActivator))
		return;

	g_pGameRules->EndMultiplayerGame();
}

//
// CGameText / game_text	-- NON-Localized HUD Message (use env_message to display a titles.txt message)
//	Flag: All players					SF_ENVTEXT_ALLPLAYERS
//

#define SF_ENVTEXT_ALLPLAYERS 0x0001

class CGameText : public CRulePointEntity
{
public:
	void Use(CBaseEntity *pActivator, CBaseEntity *pCaller, USE_TYPE useType, float value);
	void KeyValue(KeyValueData *pkvd);

	virtual int Save(CSave &save);
	virtual int Restore(CRestore &restore);
	static TYPEDESCRIPTION m_SaveData[];

	inline void MessageSet(const char *pMessage) { pev->message = ALLOC_STRING(pMessage); }
	inline const char *MessageGet(void) { return STRING(pev->message); }

private:
	hudtextparms_t m_textParms;
	int m_teamfilter;
};

LINK_ENTITY_TO_CLASS(game_text, CGameText);

// Save parms as a block.  Will break save/restore if the structure changes, but this entity didn't ship with Half-Life, so
// it can't impact saved Half-Life games.
TYPEDESCRIPTION CGameText::m_SaveData[] = {
	DEFINE_ARRAY(CGameText, m_textParms, FIELD_CHARACTER, sizeof(hudtextparms_t)),
};

IMPLEMENT_SAVERESTORE(CGameText, CRulePointEntity);

void CGameText::KeyValue(KeyValueData *pkvd)
{
	if (FStrEq(pkvd->szKeyName, "channel"))
	{
		m_textParms.channel = atoi(pkvd->szValue);
		pkvd->fHandled = TRUE;
	}
	else if (FStrEq(pkvd->szKeyName, "x"))
	{
		m_textParms.x = atof(pkvd->szValue);
		pkvd->fHandled = TRUE;
	}
	else if (FStrEq(pkvd->szKeyName, "y"))
	{
		m_textParms.y = atof(pkvd->szValue);
		pkvd->fHandled = TRUE;
	}
	else if (FStrEq(pkvd->szKeyName, "effect"))
	{
		m_textParms.effect = atoi(pkvd->szValue);
		pkvd->fHandled = TRUE;
	}
	else if (FStrEq(pkvd->szKeyName, "color"))
	{
		int color[4];
		UTIL_StringToIntArray(color, 4, pkvd->szValue);
		m_textParms.r1 = color[0];
		m_textParms.g1 = color[1];
		m_textParms.b1 = color[2];
		m_textParms.a1 = color[3];
		pkvd->fHandled = TRUE;
	}
	else if (FStrEq(pkvd->szKeyName, "color2"))
	{
		int color[4];
		UTIL_StringToIntArray(color, 4, pkvd->szValue);
		m_textParms.r2 = color[0];
		m_textParms.g2 = color[1];
		m_textParms.b2 = color[2];
		m_textParms.a2 = color[3];
		pkvd->fHandled = TRUE;
	}
	else if (FStrEq(pkvd->szKeyName, "fadein"))
	{
		m_textParms.fadeinTime = atof(pkvd->szValue);
		pkvd->fHandled = TRUE;
	}
	else if (FStrEq(pkvd->szKeyName, "fadeout"))
	{
		m_textParms.fadeoutTime = atof(pkvd->szValue);
		pkvd->fHandled = TRUE;
	}
	else if (FStrEq(pkvd->szKeyName, "holdtime"))
	{
		m_textParms.holdTime = atof(pkvd->szValue);
		pkvd->fHandled = TRUE;
	}
	else if (FStrEq(pkvd->szKeyName, "fxtime"))
	{
		m_textParms.fxTime = atof(pkvd->szValue);
		pkvd->fHandled = TRUE;
	}
	else if (FStrEq(pkvd->szKeyName, "broadcast"))
	{
		m_teamfilter = atoi(pkvd->szValue);
		pkvd->fHandled = TRUE;
	}
	else
		CRulePointEntity::KeyValue(pkvd);
}

void CGameText::Use(CBaseEntity *pActivator, CBaseEntity *pCaller, USE_TYPE useType, float value)
{
	if (!CanFireForActivator(pActivator))
		return;

	switch ( m_teamfilter )
	{
		// Activator only
		case 1:
		{
		    if ( pActivator->IsNetClient() )
			    UTIL_HudMessage(pActivator, m_textParms, MessageGet());
		}
		break;
		// All players
		case 2:
		{
		    UTIL_HudMessageAll( m_textParms, MessageGet() );
		}
		break;
		// Human only
		case 3:
		{
			const char *splitMsg = UTIL_SplitHudMessage( MessageGet() );
			for (int i = 1; i <= gpGlobals->maxClients; i++)
			{
				CBaseEntity *pPlayer = UTIL_PlayerByIndex(i);
				if ( pPlayer && pPlayer->pev->team == ZP::TEAM_SURVIVIOR )
				    UTIL_HudMessageRaw( pPlayer, m_textParms, splitMsg );
			}
		}
		break;
		// Zombie only
		case 4:
		{
			const char *splitMsg = UTIL_SplitHudMessage( MessageGet() );
			for (int i = 1; i <= gpGlobals->maxClients; i++)
			{
				CBaseEntity *pPlayer = UTIL_PlayerByIndex(i);
				if ( pPlayer && pPlayer->pev->team == ZP::TEAM_ZOMBIE )
				    UTIL_HudMessageRaw( pPlayer, m_textParms, splitMsg );
			}
		}
		break;
	}
}

//
// CGameTeamMaster / game_team_master	-- "Masters" like multisource, but based on the team of the activator
// Only allows mastered entity to fire if the team matches my team
//
// team index (pulled from server team list "mp_teamlist"
// Flag: Remove on Fire
// Flag: Any team until set?		-- Any team can use this until the team is set (otherwise no teams can use it)
//

#define SF_TEAMMASTER_FIREONCE 0x0001
#define SF_TEAMMASTER_ANYTEAM  0x0002

class CGameTeamMaster : public CRulePointEntity
{
public:
	void KeyValue(KeyValueData *pkvd);
	void Use(CBaseEntity *pActivator, CBaseEntity *pCaller, USE_TYPE useType, float value);
	int ObjectCaps(void) { return CRulePointEntity::ObjectCaps() | FCAP_MASTER; }

	BOOL IsTriggered(CBaseEntity *pActivator);
	const char *TeamID(void);
	inline BOOL RemoveOnFire(void) { return (pev->spawnflags & SF_TEAMMASTER_FIREONCE) ? TRUE : FALSE; }
	inline BOOL AnyTeam(void) { return (pev->spawnflags & SF_TEAMMASTER_ANYTEAM) ? TRUE : FALSE; }

private:
	BOOL TeamMatch(CBaseEntity *pActivator);

	int m_teamIndex;
	USE_TYPE triggerType;
};

LINK_ENTITY_TO_CLASS(game_team_master, CGameTeamMaster);

void CGameTeamMaster::KeyValue(KeyValueData *pkvd)
{
	if (FStrEq(pkvd->szKeyName, "teamindex"))
	{
		m_teamIndex = atoi(pkvd->szValue);
		pkvd->fHandled = TRUE;
	}
	else if (FStrEq(pkvd->szKeyName, "triggerstate"))
	{
		int type = atoi(pkvd->szValue);
		switch (type)
		{
		case 0:
			triggerType = USE_OFF;
			break;
		case 2:
			triggerType = USE_TOGGLE;
			break;
		default:
			triggerType = USE_ON;
			break;
		}
		pkvd->fHandled = TRUE;
	}
	else
		CRulePointEntity::KeyValue(pkvd);
}

void CGameTeamMaster::Use(CBaseEntity *pActivator, CBaseEntity *pCaller, USE_TYPE useType, float value)
{
	if (!CanFireForActivator(pActivator))
		return;

	if (useType == USE_SET)
	{
		if (value < 0)
		{
			m_teamIndex = -1;
		}
		else
		{
			m_teamIndex = g_pGameRules->GetTeamIndex(pActivator->TeamID());
		}
		return;
	}

	if (TeamMatch(pActivator))
	{
		SUB_UseTargets(pActivator, triggerType, value);
		if (RemoveOnFire())
			UTIL_Remove(this);
	}
}

BOOL CGameTeamMaster::IsTriggered(CBaseEntity *pActivator)
{
	return TeamMatch(pActivator);
}

const char *CGameTeamMaster::TeamID(void)
{
	if (m_teamIndex < 0) // Currently set to "no team"
		return "";

	return g_pGameRules->GetIndexedTeamName(m_teamIndex); // UNDONE: Fill this in with the team from the "teamlist"
}

BOOL CGameTeamMaster::TeamMatch(CBaseEntity *pActivator)
{
	if (m_teamIndex < 0 && AnyTeam())
		return TRUE;

	if (!pActivator)
		return FALSE;

	return UTIL_TeamsMatch(pActivator->TeamID(), TeamID());
}

//
// CGameTeamSet / game_team_set	-- Changes the team of the entity it targets to the activator's team
// Flag: Fire once
// Flag: Clear team				-- Sets the team to "NONE" instead of activator

#define SF_TEAMSET_FIREONCE  0x0001
#define SF_TEAMSET_CLEARTEAM 0x0002

class CGameTeamSet : public CRulePointEntity
{
public:
	void Use(CBaseEntity *pActivator, CBaseEntity *pCaller, USE_TYPE useType, float value);
	inline BOOL RemoveOnFire(void) { return (pev->spawnflags & SF_TEAMSET_FIREONCE) ? TRUE : FALSE; }
	inline BOOL ShouldClearTeam(void) { return (pev->spawnflags & SF_TEAMSET_CLEARTEAM) ? TRUE : FALSE; }

private:
};

LINK_ENTITY_TO_CLASS(game_team_set, CGameTeamSet);

void CGameTeamSet::Use(CBaseEntity *pActivator, CBaseEntity *pCaller, USE_TYPE useType, float value)
{
	if (!CanFireForActivator(pActivator))
		return;

	if (ShouldClearTeam())
	{
		SUB_UseTargets(pActivator, USE_SET, -1);
	}
	else
	{
		SUB_UseTargets(pActivator, USE_SET, 0);
	}

	if (RemoveOnFire())
	{
		UTIL_Remove(this);
	}
}

//
// CGamePlayerZone / game_player_zone -- players in the zone fire my target when I'm fired
//
// Needs master?
class CGamePlayerZone : public CRuleBrushEntity
{
public:
	void KeyValue(KeyValueData *pkvd);
	void Use(CBaseEntity *pActivator, CBaseEntity *pCaller, USE_TYPE useType, float value);

	virtual int Save(CSave &save);
	virtual int Restore(CRestore &restore);
	static TYPEDESCRIPTION m_SaveData[];

private:
	string_t m_iszInTarget;
	string_t m_iszOutTarget;
	string_t m_iszInCount;
	string_t m_iszOutCount;
};

LINK_ENTITY_TO_CLASS(game_zone_player, CGamePlayerZone);
TYPEDESCRIPTION CGamePlayerZone::m_SaveData[] = {
	DEFINE_FIELD(CGamePlayerZone, m_iszInTarget, FIELD_STRING),
	DEFINE_FIELD(CGamePlayerZone, m_iszOutTarget, FIELD_STRING),
	DEFINE_FIELD(CGamePlayerZone, m_iszInCount, FIELD_STRING),
	DEFINE_FIELD(CGamePlayerZone, m_iszOutCount, FIELD_STRING),
};

IMPLEMENT_SAVERESTORE(CGamePlayerZone, CRuleBrushEntity);

void CGamePlayerZone::KeyValue(KeyValueData *pkvd)
{
	if (FStrEq(pkvd->szKeyName, "intarget"))
	{
		m_iszInTarget = ALLOC_STRING(pkvd->szValue);
		pkvd->fHandled = TRUE;
	}
	else if (FStrEq(pkvd->szKeyName, "outtarget"))
	{
		m_iszOutTarget = ALLOC_STRING(pkvd->szValue);
		pkvd->fHandled = TRUE;
	}
	else if (FStrEq(pkvd->szKeyName, "incount"))
	{
		m_iszInCount = ALLOC_STRING(pkvd->szValue);
		pkvd->fHandled = TRUE;
	}
	else if (FStrEq(pkvd->szKeyName, "outcount"))
	{
		m_iszOutCount = ALLOC_STRING(pkvd->szValue);
		pkvd->fHandled = TRUE;
	}
	else
		CRuleBrushEntity::KeyValue(pkvd);
}

void CGamePlayerZone::Use(CBaseEntity *pActivator, CBaseEntity *pCaller, USE_TYPE useType, float value)
{
	int playersInCount = 0;
	int playersOutCount = 0;

	if (!CanFireForActivator(pActivator))
		return;

	CBaseEntity *pPlayer = NULL;

	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		pPlayer = UTIL_PlayerByIndex(i);
		if (pPlayer)
		{
			TraceResult trace;
			int hullNumber;

			hullNumber = human_hull;
			if (pPlayer->pev->flags & FL_DUCKING)
			{
				hullNumber = head_hull;
			}

			UTIL_TraceModel(pPlayer->pev->origin, pPlayer->pev->origin, hullNumber, edict(), &trace);

			if (trace.fStartSolid)
			{
				playersInCount++;
				if (m_iszInTarget)
				{
					FireTargets(STRING(m_iszInTarget), pPlayer, pActivator, useType, value);
				}
			}
			else
			{
				playersOutCount++;
				if (m_iszOutTarget)
				{
					FireTargets(STRING(m_iszOutTarget), pPlayer, pActivator, useType, value);
				}
			}
		}
	}

	if (m_iszInCount)
	{
		FireTargets(STRING(m_iszInCount), pActivator, this, USE_SET, playersInCount);
	}

	if (m_iszOutCount)
	{
		FireTargets(STRING(m_iszOutCount), pActivator, this, USE_SET, playersOutCount);
	}
}

//
// CGamePlayerHurt / game_player_hurt	-- Damages the player who fires it
// Flag: Fire once

#define SF_PKILL_FIREONCE 0x0001
class CGamePlayerHurt : public CRulePointEntity
{
public:
	void Use(CBaseEntity *pActivator, CBaseEntity *pCaller, USE_TYPE useType, float value);
	inline BOOL RemoveOnFire(void) { return (pev->spawnflags & SF_PKILL_FIREONCE) ? TRUE : FALSE; }

private:
};

LINK_ENTITY_TO_CLASS(game_player_hurt, CGamePlayerHurt);

void CGamePlayerHurt::Use(CBaseEntity *pActivator, CBaseEntity *pCaller, USE_TYPE useType, float value)
{
	if (!CanFireForActivator(pActivator))
		return;

	if (pActivator->IsPlayer())
	{
		if (pev->dmg < 0)
			pActivator->TakeHealth(-pev->dmg, DMG_GENERIC);
		else
			pActivator->TakeDamage(pev, pev, pev->dmg, DMG_GENERIC);
	}

	SUB_UseTargets(pActivator, useType, value);

	if (RemoveOnFire())
	{
		UTIL_Remove(this);
	}
}

//
// CGameTimer / game_timer	-- Game Timer, a clone of logic_timer from Source Engine
// Flag: Fire once
// Flag: Reset on Fire

class CGameTimer : public CRulePointEntity
{
public:
	void KeyValue(KeyValueData *pkvd);
	void Spawn(void);
	void Restart(void);
	void Use(CBaseEntity *pActivator, CBaseEntity *pCaller, USE_TYPE useType, float value);
	void OnScriptCallBack( KeyValues *pData );

private:
	void StartTimer();
	void StopTimer();
	void OnTimerThink();

	bool m_bDisabled = false;
	float m_flTime = -1;
	float m_flMinTime = 0.5f;
	float m_flMaxTime = 1.0f;
	int m_iRetryAmount = 1;
};

LINK_ENTITY_TO_CLASS( game_timer, CGameTimer );

void CGameTimer::Spawn(void)
{
	CRulePointEntity::Spawn();

#ifdef SCRIPT_SYSTEM
	// Inputs
	ScriptSystem::RegisterScriptCallback( AvailableScripts_t::InputOutput, this, "Activate" );
	ScriptSystem::RegisterScriptCallback( AvailableScripts_t::InputOutput, this, "DeActivate" );
	SetEntityScriptCallback( &CGameTimer::OnScriptCallBack );
#endif

	if ( (pev->spawnflags & SF_GAMETIMER_DISABLED) )
		m_bDisabled = true;
	else
		m_bDisabled = false;

	if ( !(pev->spawnflags & SF_GAMETIMER_START_ON_ROUND) )
		return;

	if ( !m_bDisabled )
		StartTimer();
}

void CGameTimer::OnScriptCallBack( KeyValues *pData )
{
	const char *szAction = pData->GetString( "Action" );
	const char *szValue = pData->GetString( "arg0" );
	// Check what kind of action we got
	if ( FStrEq( szAction, "Activate" ) )
		StartTimer();
	else if ( FStrEq( szAction, "DeActivate" ) )
		StopTimer();
}

void CGameTimer::Restart()
{
	StopTimer();

	if ( (pev->spawnflags & SF_GAMETIMER_DISABLED) )
		m_bDisabled = true;
	else
		m_bDisabled = false;

	if ( !(pev->spawnflags & SF_GAMETIMER_START_ON_ROUND) )
		return;

	if ( !m_bDisabled )
		StartTimer();
}

void CGameTimer::KeyValue(KeyValueData *pkvd)
{
	if (FStrEq(pkvd->szKeyName, "time_min"))
	{
		m_flMinTime = atof(pkvd->szValue);
		pkvd->fHandled = TRUE;
	}
	else if (FStrEq(pkvd->szKeyName, "time_max"))
	{
		m_flMaxTime = atof(pkvd->szValue);
		pkvd->fHandled = TRUE;
	}
	else if (FStrEq(pkvd->szKeyName, "amount"))
	{
		m_iRetryAmount = atoi(pkvd->szValue);
		pkvd->fHandled = TRUE;
	}
	else if (FStrEq(pkvd->szKeyName, "target"))
	{
		pev->target = ALLOC_STRING(pkvd->szValue);
		pkvd->fHandled = TRUE;
	}
	else
		CRulePointEntity::KeyValue(pkvd);
}

void CGameTimer::StartTimer()
{
	m_flTime = gpGlobals->time + RANDOM_FLOAT( m_flMinTime, m_flMaxTime );
	SetThink(&CGameTimer::OnTimerThink);
	pev->nextthink = gpGlobals->time + 0.1f;
}

void CGameTimer::StopTimer()
{
	SetThink(NULL);
}

void CGameTimer::OnTimerThink()
{
	if ( m_flTime - gpGlobals->time <= 0 )
	{
		// Fire our target.
		SUB_UseTargets( this, USE_TOGGLE, 0 );

		if ( m_iRetryAmount == 0 )
		{
			StopTimer();
			return;
		}

		if ( m_iRetryAmount > 0 )
			m_iRetryAmount--;

		StartTimer();
	}
	pev->nextthink = gpGlobals->time + 0.1f;
}

void CGameTimer::Use(CBaseEntity *pActivator, CBaseEntity *pCaller, USE_TYPE useType, float value)
{
	if ( useType == USE_SET )
	{
		StartTimer();
		return;
	}

	if ( m_bDisabled )
		StartTimer();
	else
		StopTimer();
	// Toggle it
	m_bDisabled = !m_bDisabled;
}

//
// hud_time -- Fires a target when the timer runs out, and also shows on our HUD.
// Was created for zpo_keretti.
//

class CHudTimer : public CRulePointEntity
{
public:
	void KeyValue(KeyValueData *pkvd);
	void Spawn(void);
	void Restart(void);
	void Use(CBaseEntity *pActivator, CBaseEntity *pCaller, USE_TYPE useType, float value);
	void OnScriptCallBack( KeyValues *pData );

private:
	void StartTimer();
	void StopTimer();
	void OnTimerThink();

	bool m_bDisabled = false;
	float m_flTime = -1;
	float m_flValue = 60.0f;
};

LINK_ENTITY_TO_CLASS( hud_time, CHudTimer );


void CHudTimer::Spawn(void)
{
	CRulePointEntity::Spawn();

#ifdef SCRIPT_SYSTEM
	// Inputs
	ScriptSystem::RegisterScriptCallback( AvailableScripts_t::InputOutput, this, "Activate" );
	ScriptSystem::RegisterScriptCallback( AvailableScripts_t::InputOutput, this, "DeActivate" );
	SetEntityScriptCallback( &CHudTimer::OnScriptCallBack );
#endif
}

void CHudTimer::OnScriptCallBack(KeyValues *pData)
{
	const char *szAction = pData->GetString( "Action" );
	const char *szValue = pData->GetString( "arg0" );
	// Check what kind of action we got
	if ( FStrEq( szAction, "Activate" ) )
		StartTimer();
	else if ( FStrEq( szAction, "DeActivate" ) )
		StopTimer();
}

void CHudTimer::Restart()
{
	StopTimer();
}

void CHudTimer::KeyValue(KeyValueData *pkvd)
{
	if (FStrEq(pkvd->szKeyName, "time"))
	{
		m_flValue = atof(pkvd->szValue);
		pkvd->fHandled = TRUE;
	}
	else if (FStrEq(pkvd->szKeyName, "target"))
	{
		pev->target = ALLOC_STRING(pkvd->szValue);
		pkvd->fHandled = TRUE;
	}
	else
		CRulePointEntity::KeyValue(pkvd);
}

void CHudTimer::StartTimer()
{
	m_flTime = gpGlobals->time + m_flValue;
	SetThink(&CHudTimer::OnTimerThink);
	pev->nextthink = gpGlobals->time + 0.1f;

	// Fire the event to all players
	MESSAGE_BEGIN( MSG_ALL, gmsgHudTimer );
	WRITE_BYTE( 1 );
	WRITE_FLOAT( m_flValue );
	MESSAGE_END();
}

void CHudTimer::StopTimer()
{
	SetThink(NULL);

	// Fire the event to all players
	MESSAGE_BEGIN( MSG_ALL, gmsgHudTimer );
	WRITE_BYTE( 0 );
	WRITE_FLOAT( m_flValue );
	MESSAGE_END();
}

void CHudTimer::OnTimerThink()
{
	if ( m_flTime - gpGlobals->time <= 0 )
	{
		// Fire our target.
		SUB_UseTargets( this, USE_TOGGLE, 0 );
		StopTimer();
	}
	pev->nextthink = gpGlobals->time + 0.1f;
}

void CHudTimer::Use(CBaseEntity *pActivator, CBaseEntity *pCaller, USE_TYPE useType, float value)
{
	if ( useType == USE_SET )
	{
		StartTimer();
		return;
	}

	if ( m_bDisabled )
		StartTimer();
	else
		StopTimer();
	// Toggle it
	m_bDisabled = !m_bDisabled;
}


//
// Entity Teleport
//
#define SF_TELEPORT_DST_DISABLE_ON_TELE (1 << 0) // Disable on teleport. This will auto disable the teleport

class CEntityTeleport : public CRulePointEntity
{
public:
	void KeyValue( KeyValueData *pkvd );
	void Spawn( void );
	void Use( CBaseEntity *pActivator, CBaseEntity *pCaller, USE_TYPE useType, float value ) override;
	void TeleportEntity( edict_t *pEnt, edict_t *pDest );
};
LINK_ENTITY_TO_CLASS( ent_teleport, CEntityTeleport );

void CEntityTeleport::Spawn( void )
{
	CRulePointEntity::Spawn();
}

void CEntityTeleport::KeyValue( KeyValueData *pkvd )
{
	if (FStrEq(pkvd->szKeyName, "entity"))
	{
		pev->message = ALLOC_STRING( pkvd->szValue );
		pkvd->fHandled = TRUE;
	}
	else if (FStrEq(pkvd->szKeyName, "target"))
	{
		pev->target = ALLOC_STRING(pkvd->szValue);
		pkvd->fHandled = TRUE;
	}
	else
		CRulePointEntity::KeyValue(pkvd);
}

void CEntityTeleport::Use( CBaseEntity *pActivator, CBaseEntity *pCaller, USE_TYPE useType, float value )
{
	// Find our entities, and add them to our list.
	std::vector<edict_t *> m_EndList;
	edict_t *pFind = FIND_ENTITY_BY_TARGETNAME( nullptr, STRING(pev->message) );
	while ( !FNullEnt( pFind ) )
	{
		m_EndList.push_back( pFind );
		pFind = FIND_ENTITY_BY_TARGETNAME( pFind, STRING(pev->message) );
	}
	if ( m_EndList.size() == 0 ) return;

	// We may have more than 1 target with the same name, find them all, and randomize which once we get.
	std::vector<edict_t *> m_RandomList;
	pFind = FIND_ENTITY_BY_TARGETNAME( nullptr, STRING(pev->target) );
	while ( !FNullEnt( pFind ) )
	{
		entvars_t *entvars = VARS( pFind );
		// only add our targets if they aren't disabled.
		if ( entvars->iuser1 == 0 )
			m_RandomList.push_back( pFind );
		pFind = FIND_ENTITY_BY_TARGETNAME( pFind, STRING(pev->target) );
	}

	if ( m_RandomList.size() == 0 )
	{
		m_EndList.clear();
		return;
	}

	// If we have a list, let's randomize the order.
	std::random_device rd;
	std::mt19937 g( rd() );
	std::shuffle( m_RandomList.begin(), m_RandomList.end(), g );

	edict_t *pEntTarget = nullptr;
	for ( size_t i = 0; i < m_EndList.size(); i++ )
	{
		// Our entity.
		edict_t *pEnt = m_EndList[ i ];

		bool bHaveEnoughDestinations = true;

		// Remove from list, so we don't teleport to the same destination.
		// But if we don't have enough, re-use the previous destination.
		if ( m_RandomList.size() == 0 ) bHaveEnoughDestinations = false;

		if ( bHaveEnoughDestinations )
		{
			// This is now our target.
			int iIndex = RandomInt( 0, m_RandomList.size() - 1 );
			pEntTarget = m_RandomList[ iIndex ];

			// Remove from our list.
			m_RandomList.erase( m_RandomList.begin() + iIndex );
		}
		else
		{
			Msg(
				"ent_teleport (%s) does not have enough teleportation destinations.\n%i entities teleported, but we don't have any remaining destination left.\nThe previous teleportation destination will be re-used.\n",
			    STRING( pev->targetname ),
				i + 1
			);
		}

		// If teleport entity gets disabled on use, disable it.
		entvars_t *entvars = VARS( pEntTarget );
		if ( (entvars->spawnflags & SF_TELEPORT_DST_DISABLE_ON_TELE) )
			entvars->iuser1 = 1;

		// Teleport the entity.
		TeleportEntity( pEnt, pEntTarget );
	}

	// Clear any remaining memory.
	m_EndList.clear();
	m_RandomList.clear();
}

void CEntityTeleport::TeleportEntity( edict_t *pEnt, edict_t *pDest )
{
	entvars_t *entvars = VARS( pEnt );

	Vector tmp = VARS( pDest )->origin;
	tmp.z++;

	UTIL_SetOrigin( entvars, tmp );

	entvars->angles = pDest->v.angles;
	entvars->fixangle = TRUE;
	entvars->velocity = entvars->basevelocity = g_vecZero;
}

//
// CGameCounter / game_counter	-- Counts events and fires target
// Flag: Fire once
// Flag: Reset on Fire

#define SF_GAMECOUNT_FIREONCE 0x0001
#define SF_GAMECOUNT_RESET    0x0002

class CGameCounter : public CRulePointEntity
{
public:
	void Spawn(void);
	void Restart();
	void Use(CBaseEntity *pActivator, CBaseEntity *pCaller, USE_TYPE useType, float value);
	void OnScriptCallBack( KeyValues *pData );
	inline BOOL RemoveOnFire(void) { return (pev->spawnflags & SF_GAMECOUNT_FIREONCE) ? TRUE : FALSE; }
	inline BOOL ResetOnFire(void) { return (pev->spawnflags & SF_GAMECOUNT_RESET) ? TRUE : FALSE; }

	inline void CountUp(void) { pev->frags++; }
	inline void CountDown(void) { pev->frags--; }
	inline void ResetCount(void) { pev->frags = pev->dmg; }
	inline int CountValue(void) { return pev->frags; }
	inline int LimitValue(void) { return pev->health; }

	inline BOOL HitLimit(void) { return CountValue() == LimitValue(); }

private:
	inline void SetCountValue(int value) { pev->frags = value; }
	inline void SetInitialValue(int value) { pev->dmg = value; }
	bool m_bSoftRemoved = false;
};

LINK_ENTITY_TO_CLASS(game_counter, CGameCounter);

void CGameCounter::Spawn(void)
{
	// Save off the initial count
	SetInitialValue(CountValue());
	CRulePointEntity::Spawn();
#ifdef SCRIPT_SYSTEM
	// Inputs
	ScriptSystem::RegisterScriptCallback( AvailableScripts_t::InputOutput, this, "IncreaseValue" );
	ScriptSystem::RegisterScriptCallback( AvailableScripts_t::InputOutput, this, "DecreaseValue" );
	ScriptSystem::RegisterScriptCallback( AvailableScripts_t::InputOutput, this, "SetValue" );

	// Outputs
	ScriptSystem::RegisterScriptCallback( AvailableScripts_t::InputOutput, this, "OnValueChanged" );
	ScriptSystem::RegisterScriptCallback( AvailableScripts_t::InputOutput, this, "OnHitLimit" );

	SetEntityScriptCallback( &CGameCounter::OnScriptCallBack );
#endif
}

void CGameCounter::OnScriptCallBack( KeyValues *pData )
{
	const char *szAction = pData->GetString( "Action" );
	const char *szValue = pData->GetString( "arg0" );
	// Check what kind of action we got
	if ( FStrEq( szAction, "IncreaseValue" ) )
		Use( NULL, NULL, USE_TOGGLE, 0 );
	else if ( FStrEq( szAction, "DecreaseValue" ) )
		Use( NULL, NULL, USE_OFF, 0 );
	else if ( FStrEq( szAction, "SetValue" ) )
		Use( NULL, NULL, USE_SET, atoi( szValue ) );
}

void CGameCounter::Restart()
{
	// Reset the count
	ResetCount();
	m_bSoftRemoved = false;
}

void CGameCounter::Use(CBaseEntity *pActivator, CBaseEntity *pCaller, USE_TYPE useType, float value)
{
	if (m_bSoftRemoved)
		return;
	if (!CanFireForActivator(pActivator))
		return;

	switch (useType)
	{
	case USE_ON:
	case USE_TOGGLE:
		CountUp();
		break;

	case USE_OFF:
		CountDown();
		break;

	case USE_SET:
		SetCountValue((int)value);
		break;
	}

#ifdef SCRIPT_SYSTEM
	const std::string &szOutput( "OnValueChanged" );
	ScriptSystem::CallScriptDelay(
		AvailableScripts_t::InputOutput,
		nullptr,
		szOutput,
		0.0f,
		2,
		std::to_string( entindex() ),
		std::to_string( CountValue() )
	);
#endif

	if (HitLimit())
	{
		SUB_UseTargets(pActivator, USE_TOGGLE, 0);
		if (RemoveOnFire())
			m_bSoftRemoved = true;

		if (ResetOnFire())
		{
			ResetCount();
		}

#ifdef SCRIPT_SYSTEM
		const std::string &szOutput2( "OnHitLimit" );
		ScriptSystem::CallScriptDelay(
			AvailableScripts_t::InputOutput,
			nullptr,
		    szOutput2,
			0.0f,
			1,
			std::to_string( entindex() )
		);
#endif
	}
}

//
// CGameCounterSet / game_counter_set	-- Sets the counter's value
// Flag: Fire once

#define SF_GAMECOUNTSET_FIREONCE 0x0001

class CGameCounterSet : public CRulePointEntity
{
public:
	void Use(CBaseEntity *pActivator, CBaseEntity *pCaller, USE_TYPE useType, float value);
	inline void Restart() { m_bSoftRemoved = false; }
	inline BOOL RemoveOnFire(void) { return (pev->spawnflags & SF_GAMECOUNTSET_FIREONCE) ? TRUE : FALSE; }

private:
	bool m_bSoftRemoved = false;
};

LINK_ENTITY_TO_CLASS(game_counter_set, CGameCounterSet);

void CGameCounterSet::Use(CBaseEntity *pActivator, CBaseEntity *pCaller, USE_TYPE useType, float value)
{
	if (m_bSoftRemoved)
		return;
	if (!CanFireForActivator(pActivator))
		return;

	SUB_UseTargets(pActivator, USE_SET, pev->frags);

	if (RemoveOnFire())
		m_bSoftRemoved = true;
}

//
// CGamePlayerEquip / game_playerequip	-- Sets the default player equipment
// Flag: USE Only

#define SF_PLAYEREQUIP_USEONLY 0x0001
#define MAX_EQUIP              32

class CGamePlayerEquip : public CRulePointEntity
{
public:
	void KeyValue(KeyValueData *pkvd);
	void Touch(CBaseEntity *pOther);
	void Use(CBaseEntity *pActivator, CBaseEntity *pCaller, USE_TYPE useType, float value);

	inline BOOL UseOnly(void) { return (pev->spawnflags & SF_PLAYEREQUIP_USEONLY) ? TRUE : FALSE; }

private:
	void EquipPlayer(CBaseEntity *pPlayer);

	string_t m_weaponNames[MAX_EQUIP];
	int m_weaponCount[MAX_EQUIP];
};

LINK_ENTITY_TO_CLASS(game_player_equip_punish, CGamePlayerEquip);
LINK_ENTITY_TO_CLASS(game_player_equip, CGamePlayerEquip);

void CGamePlayerEquip::KeyValue(KeyValueData *pkvd)
{
	CRulePointEntity::KeyValue(pkvd);

	if (!pkvd->fHandled)
	{
		for (int i = 0; i < MAX_EQUIP; i++)
		{
			if (!m_weaponNames[i])
			{
				char tmp[128];

				UTIL_StripToken(pkvd->szKeyName, tmp, sizeof(tmp));

				m_weaponNames[i] = ALLOC_STRING(tmp);
				m_weaponCount[i] = atoi(pkvd->szValue);
				m_weaponCount[i] = max(1, m_weaponCount[i]);
				pkvd->fHandled = TRUE;
				break;
			}
		}
	}
}

void CGamePlayerEquip::Touch(CBaseEntity *pOther)
{
	if (!CanFireForActivator(pOther))
		return;

	if (UseOnly())
		return;

	EquipPlayer(pOther);
}

void CGamePlayerEquip::EquipPlayer(CBaseEntity *pEntity)
{
	CBasePlayer *pPlayer = NULL;

	if (pEntity->IsPlayer())
	{
		pPlayer = (CBasePlayer *)pEntity;
	}

	if (!pPlayer)
		return;

	for (int i = 0; i < MAX_EQUIP; i++)
	{
		if (!m_weaponNames[i])
			break;
		for (int j = 0; j < m_weaponCount[i]; j++)
		{
			pPlayer->GiveNamedItem(STRING(m_weaponNames[i]));
		}
	}
}

void CGamePlayerEquip::Use(CBaseEntity *pActivator, CBaseEntity *pCaller, USE_TYPE useType, float value)
{
	EquipPlayer(pActivator);
}

//
// CGamePlayerTeam / game_player_team	-- Changes the team of the player who fired it
// Flag: Fire once
// Flag: Kill Player
// Flag: Gib Player

#define SF_PTEAM_FIREONCE 0x0001
#define SF_PTEAM_KILL     0x0002
#define SF_PTEAM_GIB      0x0004

class CGamePlayerTeam : public CRulePointEntity
{
public:
	void Use(CBaseEntity *pActivator, CBaseEntity *pCaller, USE_TYPE useType, float value);

private:
	inline BOOL RemoveOnFire(void) { return (pev->spawnflags & SF_PTEAM_FIREONCE) ? TRUE : FALSE; }
	inline BOOL ShouldKillPlayer(void) { return (pev->spawnflags & SF_PTEAM_KILL) ? TRUE : FALSE; }
	inline BOOL ShouldGibPlayer(void) { return (pev->spawnflags & SF_PTEAM_GIB) ? TRUE : FALSE; }

	const char *TargetTeamName(const char *pszTargetName);
};

LINK_ENTITY_TO_CLASS(game_player_team, CGamePlayerTeam);

const char *CGamePlayerTeam::TargetTeamName(const char *pszTargetName)
{
	CBaseEntity *pTeamEntity = NULL;

	while ((pTeamEntity = UTIL_FindEntityByTargetname(pTeamEntity, pszTargetName)) != NULL)
	{
		if (FClassnameIs(pTeamEntity->pev, "game_team_master"))
			return pTeamEntity->TeamID();
	}

	return NULL;
}

void CGamePlayerTeam::Use(CBaseEntity *pActivator, CBaseEntity *pCaller, USE_TYPE useType, float value)
{
	if (!CanFireForActivator(pActivator))
		return;

	if (pActivator->IsPlayer())
	{
		const char *pszTargetTeam = TargetTeamName(STRING(pev->target));
		if (pszTargetTeam)
		{
			CBasePlayer *pPlayer = (CBasePlayer *)pActivator;
			g_pGameRules->ChangePlayerTeam(pPlayer, pszTargetTeam, ShouldKillPlayer(), ShouldGibPlayer());
			if ( ZPGameRules() )
				ZPGameRules()->SendPlayerTeamInfo( pPlayer );
		}
	}

	if (RemoveOnFire())
	{
		UTIL_Remove(this);
	}
}
