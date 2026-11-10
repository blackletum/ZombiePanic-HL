// ============== Copyright (c) 2025 Monochrome Games ============== \\

#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "player.h"
#include "weapons.h"
#include "gamerules.h"
#include "zp_gamerules.h"
#include "game.h"
#include "shake.h"
#include "zp/info_random_base.h"
#include "zp/info_beacon.h"
#ifdef SCRIPT_SYSTEM
#include "core.h"
#endif

extern DLL_GLOBAL BOOL g_fGameOver;
extern uint64 g_ulCurrentWorkshopID;

// Our max rounds! (before we change to another level)
extern cvar_t roundlimit;
extern ConVar sv_fafo_only;

extern unsigned short m_usResetDecals;
extern int gmsgZPAPICall;

extern void CleanupBodyQue();

static ConVar sv_disable_zpoverrides( "sv_disable_zpoverrides", "0", FCVAR_CHEATS, "Disables Zombie Panic! specific rules." );

static const char *s_EntitiesRestarts[] = {
	"prop_objective",
	"cycler",
	"cycler_sprite",
	"light",
	"func_breakable",
	"func_door",
	"func_button",
	"func_rot_button",
	"env_fog",
	"env_render",
	"env_spark",
	"trigger_push",
	"func_water",
	"func_door_rotating",
	"func_tracktrain",
	"func_vehicle",
	"func_train",
	"func_pushable",
	"prop_pushable",
	"ambient_generic",
	"env_sprite",
	"trigger_once",
	"func_wall",
	"func_wall_toggle",
	"func_illusionary",
	"func_healthcharger",
	"func_recharge",
	"trigger_hurt",
	"multisource",
	"env_beam",
	"env_laser",
	"info_teleport_destination",
	"trigger_relay",
	"trigger_auto",
	"trigger_multiple",
	"trigger_escape",
	"trigger_capture_point",
	"info_objective",
	"info_beacon",
	"multi_manager",
	"multi_relay",
	"game_counter",
	"game_counter_set",
	"game_timer",
	"path_track",
	"info_player_team1",
	"info_player_team2",
	"hud_time",
	"", // END Marker
};

CZombiePanicGameRules::CZombiePanicGameRules()
{
	m_DisableDeathMessages = FALSE;
	m_DisableDeathPenalty = FALSE;
	m_bHasPickedVolunteer = false;
	m_bCheatsOnThisSession = false;
	m_bHostHasJoined = false;
	m_bDeferringRoundResetBroadcasts = false;
	m_bProcessingDeferredBroadcasts = false;
	m_flRoundRestartDelay = -1;
	m_flRoundJustBegun = -1;
	m_Volunteers.clear();

	// Saved objective state, for clients.
	m_savedobjtext[0] = m_savedobjtext[1] = m_savedobjstate = 0;

	m_flNextAPICallTime = gpGlobals->time + 5.0f;

	m_iRounds = 1;

	m_GameModeType = ZP::IsValidGameModeMap( STRING( gpGlobals->mapname ) );
	switch ( m_GameModeType )
	{
		case ZP::GAMEMODE_SURVIVAL: m_pGameMode = new ZPGameMode_Survival; break;
		case ZP::GAMEMODE_OBJECTIVE: m_pGameMode = new ZPGameMode_Objective; break;
		case ZP::GAMEMODE_HARDCORE: m_pGameMode = new ZPGameMode_HardCore; break;
		default: m_pGameMode = new ZPGameMode_Dev; break;
	}
	ZP::SetCurrentGameMode( m_pGameMode );
}

CZombiePanicGameRules::~CZombiePanicGameRules()
{
	m_vecDeferredBroadcasts.clear();
	m_vecAchievementAnnounces.clear();
	ZP::SetCurrentGameMode( nullptr );
}

#include "voice_gamemgr.h"
extern CVoiceGameMgr g_VoiceGameMgr;
extern cvar_t timeleft;

void CZombiePanicGameRules ::Think(void)
{
	CGameRules::Think();

	// Check if cheats were enabled this session
	CheckCheats();

#ifdef SCRIPT_SYSTEM
	ScriptSystem::OnThink();
#endif

	g_VoiceGameMgr.Update(gpGlobals->frametime);

	for ( size_t i = 0; i < m_vecAchievementAnnounces.size(); i++ )
	{
		AchievementAnnounce_t &announce = m_vecAchievementAnnounces[ i ];
		if ( announce.m_flDelay - gpGlobals->time <= 0 )
		{
			// Time to announce the achievement
			CBasePlayer *pPlayer = (CBasePlayer *)UTIL_PlayerByIndex( announce.m_iPlayerIndex );
			if ( pPlayer )
				pPlayer->NotifyOfEarnedAchivement( announce.m_iAchievementID );
			m_vecAchievementAnnounces.erase( m_vecAchievementAnnounces.begin() + i );
			// Only one announcement per frame. We do NOT want an overflow.
			break;
		}
	}

	ProcessDeferredBroadcasts();

	if ( g_fGameOver ) // someone else quit the game already
	{
		BaseClass::Think();
		return;
	}

	IGameModeBase::WinState_e eWinState = m_pGameMode->GetWinState();

	static int last_time;

	int time_remaining = 0;

	float flTimeLimit = CVAR_GET_FLOAT( "mp_timelimit" ) * 60;
	time_remaining = (int)(flTimeLimit ? (flTimeLimit - gpGlobals->time) : 0);
	if ( flTimeLimit != 0 && gpGlobals->time >= flTimeLimit && eWinState >= IGameModeBase::WinState_e::State_Draw )
	{
		GoToIntermission();
		return;
	}

	// Updates once per second
	if (timeleft.value != last_time)
	{
		if ( time_remaining <= 0 )
			time_remaining = 0;
		g_engfuncs.pfnCvar_DirectSet(&timeleft, UTIL_VarArgs("%i", time_remaining));
	}

	last_time = time_remaining;

	if ( eWinState >= IGameModeBase::WinState_e::State_Draw || m_pGameMode->GetRoundState() == ZP::RoundState::RoundState_RoundIsOver )
	{
		m_pGameMode->SetRoundState( ZP::RoundState::RoundState_RoundIsOver );
		ResetRound();
		return;
	}

	// We do the gamemode thinking on a seperate class, to make it less
	// of a mess.
	m_pGameMode->OnGameModeThink();

	switch ( m_pGameMode->GetRoundState() )
	{
		case ZP::RoundState::RoundState_PickVolunteers: PickRandomVolunteer(); break;
	}

	// We do API calls processing here at the very end.
	ProcessAPICalls();
}

extern int gmsgGameMode;
extern int gmsgSayText;
extern int gmsgTeamInfo;
extern int gmsgTeamNames;
extern int gmsgScoreInfo;
extern int gmsgSpectator;
extern int gmsgRounds;
extern int gmsgRoundResetPre;
extern int gmsgRoundResetPost;
extern int gmsgObjective;

void CZombiePanicGameRules::QueueDeferredBroadcast( DeferredBroadcastType type, int playerIndex, int value )
{
	for ( size_t i = 0; i < m_vecDeferredBroadcasts.size(); i++ )
	{
		DeferredBroadcast_t &broadcast = m_vecDeferredBroadcasts[i];
		if ( broadcast.Type == type && broadcast.PlayerIndex == playerIndex )
		{
			broadcast.Value = value;
			return;
		}
	}

	DeferredBroadcast_t broadcast = { type, playerIndex, value };
	m_vecDeferredBroadcasts.push_back( broadcast );
}

void CZombiePanicGameRules::ProcessDeferredBroadcasts()
{
	if ( m_vecDeferredBroadcasts.empty() )
		return;

	constexpr size_t MAX_BROADCASTS_PER_FRAME = 4;
	size_t broadcastCount = 0;

	while ( broadcastCount < MAX_BROADCASTS_PER_FRAME && !m_vecDeferredBroadcasts.empty() )
	{
		const DeferredBroadcast_t broadcast = m_vecDeferredBroadcasts.front();
		m_vecDeferredBroadcasts.erase( m_vecDeferredBroadcasts.begin() );
		m_bProcessingDeferredBroadcasts = true;

		switch ( broadcast.Type )
		{
			case DeferredBroadcastType::TeamInfo:
			{
				CBasePlayer *pPlayer = (CBasePlayer *)UTIL_PlayerByIndex( broadcast.PlayerIndex );
				if ( pPlayer && pPlayer->IsConnected() )
				{
					MESSAGE_BEGIN( MSG_ALL, gmsgTeamInfo );
					WRITE_BYTE( pPlayer->entindex() );
					WRITE_STRING( pPlayer->pev->iuser1 ? "" : pPlayer->TeamID() );
					MESSAGE_END();
				}
				break;
			}

			case DeferredBroadcastType::ScoreInfo:
			{
				CBasePlayer *pPlayer = (CBasePlayer *)UTIL_PlayerByIndex( broadcast.PlayerIndex );
				if ( pPlayer && pPlayer->IsConnected() )
				{
					MESSAGE_BEGIN( MSG_ALL, gmsgScoreInfo );
					WRITE_BYTE( pPlayer->entindex() );
					WRITE_SHORT( pPlayer->pev->frags );
					WRITE_SHORT( pPlayer->m_iDeaths );
					WRITE_SHORT( 0 );
					WRITE_SHORT( pPlayer->pev->team );
					MESSAGE_END();
				}
				break;
			}

			case DeferredBroadcastType::Spectator:
				MESSAGE_BEGIN( MSG_ALL, gmsgSpectator );
				WRITE_BYTE( broadcast.PlayerIndex );
				WRITE_BYTE( broadcast.Value );
				MESSAGE_END();
				break;

			case DeferredBroadcastType::RoundResetPost:
				MESSAGE_BEGIN( MSG_ALL, gmsgRoundResetPost, NULL );
				WRITE_BYTE( broadcast.Value );
				MESSAGE_END();
				break;
		}

		m_bProcessingDeferredBroadcasts = false;

		broadcastCount++;
	}
}

void CZombiePanicGameRules::UpdateGameMode(CBasePlayer *pPlayer)
{
	if ( !pPlayer ) return;
	if ( !pPlayer->IsConnected() ) return;
	MESSAGE_BEGIN(MSG_ONE, gmsgGameMode, NULL, pPlayer->edict());
	WRITE_BYTE(m_GameModeType);
	MESSAGE_END();
	MESSAGE_BEGIN(MSG_ONE, gmsgRounds, NULL, pPlayer->edict());
	WRITE_BYTE(m_iRounds);
	MESSAGE_END();
}

const char *CZombiePanicGameRules::SetDefaultPlayerTeam(CBasePlayer *pPlayer)
{
	return pPlayer->TeamID();
}

//=========================================================
// InitHUD
//=========================================================
void CZombiePanicGameRules::InitHUD(CBasePlayer *pPlayer)
{
	if ( !pPlayer ) return;
	if ( !pPlayer->IsConnected() ) return;
	BaseClass::InitHUD(pPlayer);

	// update the current player of the team he is joining
	char text[256];

	// Send down the team names
	MESSAGE_BEGIN(MSG_ONE, gmsgTeamNames, NULL, pPlayer->edict());
	WRITE_BYTE(ZP::MAX_TEAM);
	for (int i = 0; i < ZP::MAX_TEAM; i++)
	{
		WRITE_STRING(ZP::Teams[i]);
	}
	MESSAGE_END();

	ChangePlayerTeam(pPlayer, ZP::Teams[ZP::TEAM_OBSERVER], FALSE, FALSE);
	SendPlayerTeamInfo(pPlayer);

	// loop through all active players and send their team info to the new client
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		CBaseEntity *plr = UTIL_PlayerByIndex(i);
		if (plr && IsValidTeam(plr->TeamID()))
		{
			MESSAGE_BEGIN(MSG_ONE, gmsgTeamInfo, NULL, pPlayer->edict());
			WRITE_BYTE(plr->entindex());
			WRITE_STRING(plr->pev->iuser1 ? "" : plr->TeamID());
			MESSAGE_END();
		}
	}

	m_pGameMode->OnHUDInit( pPlayer );
}

void CZombiePanicGameRules::ChangePlayerTeam(CBasePlayer *pPlayer, const char *pTeamName, BOOL bKill, BOOL bGib)
{
	int clientIndex = pPlayer->entindex();

	if (bKill)
	{
		// kill the player,  remove a death,  and let them start on the new team
		m_DisableDeathMessages = TRUE;
		m_DisableDeathPenalty = TRUE;

		int damageFlags = DMG_GENERIC | (bGib ? DMG_ALWAYSGIB : DMG_NEVERGIB);
		pPlayer->TakeDamage(pPlayer->pev, pPlayer->pev, 10000, damageFlags);

		m_DisableDeathMessages = FALSE;
		m_DisableDeathPenalty = FALSE;
	}

	// Set team to player
	pPlayer->pev->team = GetTeamIndex( pTeamName );
}

float CZombiePanicGameRules::FlPlayerFallDamage(CBasePlayer *pPlayer)
{
	int iFallDamage = (int)falldamage.value;
	switch ( iFallDamage )
	{
		// progressive
		case 1:
		{
			pPlayer->m_flFallVelocity -= PLAYER_MAX_SAFE_FALL_SPEED;
			float flRes = pPlayer->m_flFallVelocity * DAMAGE_FOR_FALL_SPEED;
		    if ( pPlayer->pev->team == ZP::TEAM_ZOMBIE )
			    flRes = clamp( flRes, 0, 15 );
		    return flRes;
		}
		// no damage
		case 2: return 0;
		// fixed
		default: return 10;
	}
}

void CZombiePanicGameRules::SendPlayerTeamInfo(CBasePlayer *pPlayer)
{
	if ( !pPlayer ) return;
	if ( m_bDeferringRoundResetBroadcasts ) return;

	QueueDeferredBroadcast( DeferredBroadcastType::TeamInfo, pPlayer->entindex(), 0 );
	QueueDeferredBroadcast( DeferredBroadcastType::ScoreInfo, pPlayer->entindex(), 0 );
}

void CZombiePanicGameRules::SendPlayerScoreInfo(CBasePlayer *pPlayer)
{
	if ( !pPlayer ) return;
	if ( m_bDeferringRoundResetBroadcasts ) return;

	QueueDeferredBroadcast( DeferredBroadcastType::ScoreInfo, pPlayer->entindex(), 0 );
}

void CZombiePanicGameRules::SendPlayerSpectatorInfo(CBasePlayer *pPlayer, bool bIsSpectator)
{
	if ( !pPlayer ) return;

	QueueDeferredBroadcast( DeferredBroadcastType::Spectator, pPlayer->entindex(), bIsSpectator ? 1 : 0 );
}

void CZombiePanicGameRules::ResetRound()
{
	if ( m_flRoundRestartDelay == -1 )
	{
		IGameModeBase::WinState_e winner = m_pGameMode->GetWinState();

		m_flRoundRestartDelay = gpGlobals->time + 5.0f;

		int iPlayers = 0;
		for ( int i = 1; i <= gpGlobals->maxClients; i++ )
		{
			CBasePlayer *plr = (CBasePlayer *)UTIL_PlayerByIndex( i );
			if ( plr && plr->IsConnected() )
			{
				// No more buttons
				plr->pev->button = 0;

				// If survivor, then increase the player count
				if ( plr->pev->team == ZP::TEAM_SURVIVIOR )
					iPlayers++;
			}
		}

		// Sets the data from gmsgRoundState and contains the data for all the achievement giving.
		MESSAGE_BEGIN( MSG_ALL, gmsgRoundResetPre );
		WRITE_SHORT( m_pGameMode->GetRoundState() );
		WRITE_SHORT( m_pGameMode->GetGameModeType() );
		WRITE_SHORT( winner );
		WRITE_SHORT( iPlayers );
		WRITE_BYTE( m_pGameMode->HasTimeRanOut() ? 1 : 0 );
		MESSAGE_END();

		return;
	}
	if ( m_flRoundRestartDelay - gpGlobals->time > 0 ) return;
	m_flRoundRestartDelay = -1;

	// Reset volunteers
	ResetVolunteers();

	// Check round limit. If we reached it, go to intermission
	// instead of restarting the round.
	int iRoundLimit = (int)roundlimit.value;
	if ( iRoundLimit > 0 )
	{
		// We need to change the map
		if ( m_iRounds >= iRoundLimit )
		{
			GoToIntermission();
			return;
		}
	}

	// Reset all map objects
	CleanUpMap();
	m_bDeferringRoundResetBroadcasts = true;

	// Respawn all players
	for ( int i = 1; i <= gpGlobals->maxClients; i++ )
	{
		CBasePlayer *plr = (CBasePlayer *)UTIL_PlayerByIndex( i );
		if ( plr )
		{
			plr->StopObserver();
			ChangePlayerTeam(plr, ZP::Teams[ZP::TEAM_OBSERVER], FALSE, FALSE);
			plr->StartWelcomeCam();
			plr->SetHasEscaped( false );
			// The player is now naked :)
			plr->RemoveAllItems( TRUE );
			// This is being applied in StartWelcomeCam(),
			// but we do it again, just incase if m_bInWelcomeCam
			// was never set back to FALSE.
			plr->pev->solid = SOLID_NOT;
			plr->pev->effects |= EF_NODRAW;
			// Make sure this is off.
			plr->m_bNoLives = false;
			SetPlayerModel( plr );
		}
	}

	m_pGameMode->RestartRound();

	// Increase the round value
	m_iRounds++;

	// On round reset, we set our round data, similar to gmsgRounds
	// But we also set the team menu to draw, similar to gmsgVGUIMenu
	QueueDeferredBroadcast( DeferredBroadcastType::RoundResetPost, 0, m_iRounds );
	m_bDeferringRoundResetBroadcasts = false;
}

void CZombiePanicGameRules::CleanUpMap()
{
	// Cleanup player inventories first
	for ( int i = 1; i <= gpGlobals->maxClients; i++ )
	{
		CBasePlayer *plr = (CBasePlayer *)UTIL_PlayerByIndex( i );
		if ( plr )
			plr->RemoveAllItems( TRUE );
	}

	// Release or reset everything entities in depending of flags ObjectCaps
	// (FCAP_MUST_RESET / FCAP_MUST_RELEASE)
	UTIL_ResetEntities();

	// Recreate all the map entities from the map data (preserving their indices),
	// then remove everything else except the players.
	for ( int i = 0; i < ARRAYSIZE( s_EntitiesRestarts ); i++ )
		UTIL_RestartOther( s_EntitiesRestarts[i] );

	// Make sure these are removed.
	UTIL_RemoveAll( "monster_satchel" );
	UTIL_RemoveAll( "grenade" );
	UTIL_RemoveAll( "weaponbox" );
	UTIL_RemoveAll( "bodyque" ); // We don't want any bodies on next round

	// Fix the body que on the next round
	CleanupBodyQue();

	// Spawn our static stuff
	ZP::SpawnStaticSpawns();

	// Make sure we reset our decals.
	PLAYBACK_EVENT((FEV_GLOBAL | FEV_RELIABLE), 0, m_usResetDecals);
}

void CZombiePanicGameRules::ResetVolunteers()
{
	m_bHasPickedVolunteer = false;
	m_Volunteers.clear();
	// Check if we should clear it before hand.
	m_pGameMode->ShouldClearChoosenZombies();
}

void CZombiePanicGameRules::PickRandomVolunteer()
{
	if ( m_bHasPickedVolunteer ) return;
	m_bHasPickedVolunteer = true;
	// Punish players who join late
	m_flRoundJustBegun = gpGlobals->time + 20;
	if ( m_pGameMode->IsTestModeActive() ) return;
	int iMoreRequired = 0;
	bool bFirstTry = false;

add_one_more_zombie:

	// We have no volunteers, so lets pick some random survivors
	if ( m_Volunteers.size() == 0 )
	{
		for ( int i = 1; i <= gpGlobals->maxClients; i++ )
		{
			CBasePlayer *plr = (CBasePlayer *)UTIL_PlayerByIndex(i);
			if ( plr && plr->IsAlive() && !m_pGameMode->WasAlreadyChoosenPreviously( plr ) && plr->pev->team == ZP::TEAM_SURVIVIOR )
				m_Volunteers.push_back( i );
		}
	}

	// Still zero? Reset the choosen list and try again.
	// This happens if a player is still a spectator.
	if ( m_Volunteers.size() == 0 )
	{
		if ( !bFirstTry )
		{
			bFirstTry = true;
			m_pGameMode->ClearChoosenZombies();
			// Try again.
			goto add_one_more_zombie;
		}
		return;
	}

	int iVolunteers = m_Volunteers.size() - 1;
	int iPlayerIndex = 0;
	int iVolunteerIndex = 0;
	if ( iVolunteers > 0 )
		iVolunteerIndex = RANDOM_LONG( 0, iVolunteers );
	iPlayerIndex = m_Volunteers[iVolunteerIndex];

	if ( iPlayerIndex == 0 ) return;
	m_Volunteers.erase( m_Volunteers.begin() + iVolunteerIndex );
	CBasePlayer *plr = (CBasePlayer *)UTIL_PlayerByIndex( iPlayerIndex );
	if ( plr )
	{
		ChangePlayerTeam( plr, ZP::Teams[ZP::TEAM_ZOMBIE], FALSE, FALSE );
		SendPlayerTeamInfo(plr);
		plr->Spawn();
		m_pGameMode->AddToChoosenList( plr );
		iMoreRequired--;
	}

	// Count the survivors. If we have 8 survivors, add 1 more.
	// and if we got more than 15, make sure we always have 3 zombies.
	if ( iMoreRequired == -1 )
	{
		int iSurvivors = 0;
		for ( int i = 1; i <= gpGlobals->maxClients; i++ )
		{
			CBaseEntity *plr = UTIL_PlayerByIndex(i);
			if ( plr && plr->IsAlive() && plr->pev->team == ZP::TEAM_SURVIVIOR )
				iSurvivors++;
		}

		// Make sure we start with 0 if we have 6 or more.
		// Since the current value is -1.
		if ( iSurvivors >= 6 )
			iMoreRequired = 0;

		// Now check survivors, and only increase the iMoreRequired by 1 for every 6 survivors, to prevent too many zombies from spawning at once.
		for ( int y = 1; y <= iSurvivors; y++ )
		{
			if ( y % 6 == 0 )
				iMoreRequired++;
		}
	}

	// If it's higher than 0, then add the next one.
	if ( iMoreRequired > 0 )
		goto add_one_more_zombie;
}

void CZombiePanicGameRules::PlayerSpawn(CBasePlayer *pPlayer)
{
	// Start welcome cam for new players
	if ( !pPlayer->m_bPutInServer )
	{
		// don't let him spawn as soon as he enters the server
		// give enough time to plugins to send the player to spectator mode
		pPlayer->m_flNextAttack = mp_welcomecam_delay.GetFloat();

		pPlayer->StartWelcomeCam();

		// We just joined.
		pPlayer->m_bNoLives = false;
		SetPlayerModel( pPlayer );
		return;
	}

	SetPlayerModel( pPlayer );

	int iTeamNumber = pPlayer->pev->team;
	if ( iTeamNumber == ZP::TEAM_OBSERVER ) return;

	int aws = pPlayer->m_iAutoWepSwitch;
	pPlayer->m_iAutoWepSwitch = 1;

	pPlayer->SetWeaponOwn( WEAPON_SUIT, true );

	// Zombies
	if ( iTeamNumber == ZP::TEAM_ZOMBIE )
	{
		// Zombie arms!
		// That's all that the zombies get!
		if ( sv_fafo_only.GetBool() )
			pPlayer->GiveNamedItem( "weapon_fafo" );
		else
			pPlayer->GiveNamedItem( "weapon_swipe" );

		// Zombies can't choose weapons, they only got their arms.
		pPlayer->m_iHideHUD |= HIDEHUD_WEAPONS;
	}

	FireTargets("game_playerspawn", pPlayer, pPlayer, USE_TOGGLE, 0);

	pPlayer->m_iAutoWepSwitch = aws;

	m_pGameMode->OnPlayerSpawned( pPlayer );
}

BOOL CZombiePanicGameRules::ClientCommand(CBasePlayer *pPlayer, const char *pcmd)
{
	if (FStrEq(pcmd, "joingame"))
	{
		if ( pPlayer->pev->team == ZP::TEAM_OBSERVER || pPlayer->m_bInWelcomeCam )
		{
			int iCurrent, iMax;
			m_pGameMode->GetZombieLifeData( iCurrent, iMax );

			// Go no more lives? stop.
			// This only applies if the player is on TEAM_OBSERVER
			if ( pPlayer->m_bNoLives || iCurrent == 0 )
			{
				if ( !pPlayer->IsObserver() )
					pPlayer->StartObserver();
				return TRUE;
			}

			// Stop being spec. Thanks.
			if ( pPlayer->IsObserver() )
				pPlayer->StopObserver();

			const char *pVolunteer = CMD_ARGV(1);
			if ( pVolunteer && pVolunteer[0] )
			{
				if (FStrEq(pVolunteer, "volunteer"))
					m_Volunteers.push_back( pPlayer->entindex() );
			}
			bool bLateJoin = ( m_pGameMode->GetRoundState() == ZP::RoundState::RoundState_RoundHasBegun ) ? true : false;

			// If we are late joining, make sure we activate beacons that are active.
			if ( bLateJoin )
			{
				CInfoBeacon *pBeacon = (CInfoBeacon *)UTIL_FindEntityByClassname( nullptr, "info_beacon" );
				while ( pBeacon )
				{
					pBeacon->UpdateMessageStateForEntity( pPlayer );
					pBeacon = (CInfoBeacon *)UTIL_FindEntityByClassname( pBeacon, "info_beacon" );
				}
				// Make sure to also set the current objective text.
				MESSAGE_BEGIN( MSG_ONE, gmsgObjective, NULL, pPlayer->edict() );
				WRITE_SHORT( m_savedobjstate );
				WRITE_STRING( STRING( m_savedobjtext[0] ) );
				WRITE_STRING( STRING( m_savedobjtext[1] ) );
				MESSAGE_END();
			}

			if ( bLateJoin
				&& m_flRoundJustBegun - gpGlobals->time > 0
				&& !m_pGameMode->HasLeftMidRound( pPlayer ) )
			{
				bLateJoin = false;
				pPlayer->m_bPunishLateJoiner = true;
			}

			ChangePlayerTeam(pPlayer, ZP::Teams[ bLateJoin ? ZP::TEAM_ZOMBIE : ZP::TEAM_SURVIVIOR], FALSE, FALSE);
			SendPlayerTeamInfo(pPlayer);
			pPlayer->RemoveAllItems( TRUE );
			pPlayer->StopWelcomeCam();
			if ( pPlayer->m_bPunishLateJoiner )
				m_pGameMode->GiveWeapons( pPlayer );
			if ( m_pGameMode->GetRoundState() < ZP::RoundState::RoundState_RoundHasBegun )
				pPlayer->m_iHideHUD = HIDEHUD_WEAPONS | HIDEHUD_HEALTH | HIDEHUD_FLASHLIGHT;
		}
		return TRUE;
	}
	// Grab the player position. Similar to Source Engine's "getpos" command.
	else if (FStrEq(pcmd, "getpos"))
	{
		// Get player position
		const Vector vOrigin = pPlayer->pev->origin;
		// Get player angles
		const Vector vAngles = pPlayer->pev->angles;
		// Print it to the console
		UTIL_PrintConsole(
			UTIL_VarArgs(
				"setpos \"%f %f %f %f %f %f\"\n",
				vOrigin.x, vOrigin.y, vOrigin.z,
				vAngles.x, vAngles.y, vAngles.z
			),
			pPlayer
		);
		return TRUE;
	}
	// Ditto, but for setting position. Similar to Source Engine's "setpos" command.
	else if (FStrEq(pcmd, "setpos"))
	{
		const char *pszCommand = CMD_ARGV(1);
		bool bIsCheatsEnabled = CVAR_GET_FLOAT("sv_cheats") >= 1 ? true : false;
		if ( bIsCheatsEnabled && pszCommand && pszCommand[0] )
		{
			// Set our position and angles
			float x, y, z, pitch, yaw, roll;
			int args = sscanf( pszCommand, "%f %f %f %f %f %f", &x, &y, &z, &pitch, &yaw, &roll );
			if ( args >= 3 )
			{
				pPlayer->SetOrigin( Vector( x, y, z ) );
				// If we got all 6 args, set angles too
				if ( args == 6 )
				{
					pPlayer->pev->v_angle = g_vecZero;
					pPlayer->pev->velocity = g_vecZero;
					pPlayer->SetAngles( Vector( pitch, yaw, roll ) );
					pPlayer->pev->punchangle = g_vecZero;
					pPlayer->pev->fixangle = TRUE; // turn this way immediately
				}
				pPlayer->pev->velocity = g_vecZero;
			}
			else
				UTIL_PrintConsole( "Usage: setpos \"x y z pitch yaw roll\"\n", pPlayer );
		}
		else
			UTIL_PrintConsole( "Cheats are not enabled on this server.\n", pPlayer );
		return TRUE;
	}
	else if (FStrEq(pcmd, "dropcurrentweapon"))
	{
		if ( pPlayer->pev->team == ZP::TEAM_SURVIVIOR )
			pPlayer->DropActiveWeapon();
		return TRUE;
	}
	else if (FStrEq(pcmd, "dropammo"))
	{
		if ( pPlayer->pev->team == ZP::TEAM_SURVIVIOR )
			pPlayer->DropSelectedAmmo();
		return TRUE;
	}
	else if (FStrEq(pcmd, "changeammotype"))
	{
		if ( pPlayer->pev->team == ZP::TEAM_SURVIVIOR )
			pPlayer->ChangeAmmoTypeToDrop();
		return TRUE;
	}
	else if (FStrEq(pcmd, "dua"))
	{
		if ( pPlayer->pev->team == ZP::TEAM_SURVIVIOR )
			pPlayer->DropUnuseableAmmo();
		return TRUE;
	}
	else if (FStrEq(pcmd, "panic"))
	{
		if ( pPlayer->pev->team == ZP::TEAM_SURVIVIOR )
			pPlayer->DoPanic();
		return TRUE;
	}
	// Optimzized this for dedicated servers. It caused an overflow if we have 16 players, but worked fine for Peer-2-Peer.
	// So we simply add these to a simple queue and process them in Think, to avoid the overflow.
	else if (FStrEq(pcmd, "achearn"))
	{
		const char *pAchivement = CMD_ARGV(1);
		if ( pAchivement && pAchivement[0] )
		{
			AchievementAnnounce_t announce;
			announce.m_iPlayerIndex = pPlayer->entindex();
			announce.m_iAchievementID = atoi( pAchivement );
			announce.m_flDelay = gpGlobals->time + 0.5f + m_vecAchievementAnnounces.size();
			m_vecAchievementAnnounces.push_back( announce );
		}
		return TRUE;
	}
	else if (FStrEq(pcmd, "giveammo"))
	{
		const char *pSetCommand = CMD_ARGV(1);
		bool bIsCheatsEnabled = CVAR_GET_FLOAT("sv_cheats") >= 1 ? true : false;
		if ( bIsCheatsEnabled )
			pPlayer->GiveCurrentAmmo();
		return TRUE;
	}
	else if (FStrEq(pcmd, "giveboard"))
	{
		const char *pSetCommand = CMD_ARGV(1);
		bool bIsCheatsEnabled = CVAR_GET_FLOAT("sv_cheats") >= 1 ? true : false;
		if ( bIsCheatsEnabled )
			pPlayer->PickupAmmo( 1, GetAmmoByAmmoID( AMMO_BARRICADE ) );
		return TRUE;
	}
	else if (FStrEq(pcmd, "giveitem"))
	{
		int iszItem = ALLOC_STRING( CMD_ARGV(1) ); // Make a copy of the classname
		OnWeaponGive( pPlayer, STRING(iszItem) );
		return TRUE;
	}
	else if (FStrEq(pcmd, "report_entities"))
	{
		ZP::CheckHowManySpawnedItems( pPlayer );
		return TRUE;
	}
	else if (FStrEq(pcmd, "slot"))
	{
		const char *pSelectedWeaponSlot = CMD_ARGV(1);
		if ( pSelectedWeaponSlot && pSelectedWeaponSlot[0] )
			pPlayer->SelectWeaponFromSlot( atoi( pSelectedWeaponSlot ) );
		return TRUE;
	}
	else if (FStrEq(pcmd, "invnext"))
	{
		pPlayer->SelectNextSlot();
		return TRUE;
	}
	else if (FStrEq(pcmd, "invprev"))
	{
		pPlayer->SelectPreviousSlot();
		return TRUE;
	}
	else if (FStrEq(pcmd, "_retrieve"))
	{
		const char *pSetCommand = CMD_ARGV(1);
		if ( pSetCommand && pSetCommand[0] )
		{
			const char *arg1 = CMD_ARGV(2);
			const char *arg2 = CMD_ARGV(3);
			const char *arg3 = CMD_ARGV(4);
			const char *arg4 = CMD_ARGV(5);
			const char *arg5 = CMD_ARGV(6);
			if ( FStrEq( pSetCommand, "start" ) )
			{
				// Let's compare the player private key with what we received
				if ( !FStrEq( arg1, pPlayer->GetAPIRetrieveKey() ) || FStrEq( arg1, "" ) )
				{
					pPlayer->ClearAPIRetrieveKey();
					return TRUE;
				}
				pPlayer->SetAPIPassed( true );
			}
			else if ( FStrEq( pSetCommand, "1" ) )
			{
				if ( !pPlayer->GetAPIPassed() ) return TRUE;
				int client = pPlayer->entindex();
				m_ClientsData[ client ].Game = arg1 ? (eGameAPIVersion)atoi( arg1 ) : eGameAPIVersion::k_eGameUnknown;
				m_ClientsData[ client ].Tier = arg2 ? (eSupporterTier)atoi( arg2 ) : eSupporterTier::k_eSupporterTier_NONE;
				m_ClientsData[ client ].Key = arg3 ? arg3 : "";
			}
			else if ( FStrEq( pSetCommand, "2" ) )
			{
				if ( !pPlayer->GetAPIPassed() ) return TRUE;
				int client = pPlayer->entindex();
				m_ClientsData[ client ].PanicToMelee = arg1 ? atoi( arg1 ) : false;
				m_ClientsData[ client ].KeepZVision = arg2 ? atoi( arg2 ) : false;
				m_ClientsData[ client ].AutoSwitchOnPickup = arg3 ? atoi( arg3 ) : false;
				m_ClientsData[ client ].DoScreenTint = arg4 ? atoi( arg4 ) : false;
				m_ClientsData[ client ].Character = arg5 ? arg5 : "";
			}
			else if ( FStrEq( pSetCommand, "end" ) )
			{
				if ( !pPlayer->GetAPIPassed() ) return TRUE;
				DoAPICallBack( pPlayer );
				pPlayer->ClearAPIRetrieveKey();
				// Set the model again after we get this. We may have changed "Character" value.
				pPlayer->SetTheCorrectPlayerModel();
			}
		}
		return TRUE;
	}
	else if (FStrEq(pcmd, "ent_fire"))
	{
		const char *pSetCommand = CMD_ARGV(1);
		bool bIsCheatsEnabled = CVAR_GET_FLOAT("sv_cheats") >= 1 ? true : false;
		if ( bIsCheatsEnabled && pSetCommand && pSetCommand[0] )
			FireTargets( pSetCommand, pPlayer, pPlayer, USE_TOGGLE, 0);
		return TRUE;
	}
	else if (FStrEq(pcmd, "_set"))
	{
		const char *pSetCommand = CMD_ARGV(1);
		bool bIsCheatsEnabled = CVAR_GET_FLOAT("sv_cheats") >= 1 ? true : false;
		if ( bIsCheatsEnabled
			&& pSetCommand
			&& pSetCommand[0]
			&& ( pPlayer->pev->team == ZP::TEAM_SURVIVIOR || pPlayer->pev->team == ZP::TEAM_ZOMBIE ) )
		{
			if ( FStrEq( pSetCommand, "god" ) )
			{
				// If we have the godmode flag, remove it
				if ( (pPlayer->pev->flags & FL_GODMODE) )
					pPlayer->pev->flags &= ~FL_GODMODE;
				else
					pPlayer->pev->flags |= FL_GODMODE;
				UTIL_PrintConsole( UTIL_VarArgs( "God mode has been turned %s\n", (pPlayer->pev->flags & FL_GODMODE) ? "on" : "off" ), pPlayer );
			}
			else if ( FStrEq( pSetCommand, "buddha" ) )
			{
				// If we have the godmode flag, remove it
				pPlayer->m_bBuddhaMode = !pPlayer->m_bBuddhaMode;
				UTIL_PrintConsole( UTIL_VarArgs( "Buddha mode has been turned %s\n", (pPlayer->m_bBuddhaMode) ? "on" : "off" ), pPlayer );
			}
			else if ( FStrEq( pSetCommand, "noclip" ) )
			{
				// If we have the godmode flag, remove it
				if ( pPlayer->pev->movetype == MOVETYPE_NOCLIP )
					pPlayer->pev->movetype = MOVETYPE_WALK;
				else
					pPlayer->pev->movetype = MOVETYPE_NOCLIP;
				UTIL_PrintConsole( UTIL_VarArgs( "Noclip has been turned %s\n", (pPlayer->pev->movetype == MOVETYPE_NOCLIP) ? "on" : "off" ), pPlayer );
			}
			else if ( FStrEq( pSetCommand, "explode" ) )
			{
				pPlayer->TakeDamage(pPlayer->pev, pPlayer->pev, 10000, DMG_ALWAYSGIB);
			}
			else if ( FStrEq( pSetCommand, "die_h" ) )
			{
				pPlayer->m_LastHitGroup = 1; // HITGROUP_HEAD
				pPlayer->TakeDamage(pPlayer->pev, pPlayer->pev, 10000, DMG_NEVERGIB);
				pPlayer->m_LastHitGroup = 0;
			}
		}
		return TRUE;
	}
	else if (FStrEq(pcmd, "vocalize"))
	{
		const char *szVoice = CMD_ARGV(1);
		PlayerVocalizeType nType = VOCALIZE_NONE;
		if ( szVoice && szVoice[0] )
		{
			if ( FStrEq( szVoice, "agree" ) ) nType = VOCALIZE_AGREE;
			else if ( FStrEq( szVoice, "decline" ) ) nType = VOCALIZE_DECLINE;
			else if ( FStrEq( szVoice, "cover" ) ) nType = VOCALIZE_COVER;
			else if ( FStrEq( szVoice, "need_ammo" ) ) nType = VOCALIZE_NEED_AMMO;
			else if ( FStrEq( szVoice, "need_weapon" ) ) nType = VOCALIZE_NEED_WEAPON;
			else if ( FStrEq( szVoice, "hold" ) ) nType = VOCALIZE_HOLD_HERE;
			else if ( FStrEq( szVoice, "fire" ) ) nType = VOCALIZE_OPEN_FIRE;
			else if ( FStrEq( szVoice, "taunt" ) ) nType = VOCALIZE_TAUNT;
		}
		if ( pPlayer->IsAlive() )
			pPlayer->DoVocalize( nType );
		return TRUE;
	}
#ifdef SCRIPT_SYSTEM
	else if (FStrEq(pcmd, "ss") || FStrEq(pcmd, "scriptsystem") || FStrEq(pcmd, "scall"))
	{
		const char *pSetCommand = CMD_ARGV(1);
		if ( FStrEq( pSetCommand, "func" ) )
		{
			UTIL_PrintConsole( "Available functions:\n", pPlayer );
			for ( int i = 0; i < IOFunctions_t::IO_ON_MAX; i++ )
				UTIL_PrintConsole( UTIL_VarArgs( "%s\n", IO_GetAvailableFunctions( (IOFunctions_t)i ) ), pPlayer );
			return TRUE;
		}
		else if ( FStrEq( pSetCommand, "cmds" ) )
		{
			UTIL_PrintConsole( "Available commands:\n", pPlayer );
			for ( int i = 0; i < IOFunctionCommands_t::IO_MAX; i++ )
				UTIL_PrintConsole( UTIL_VarArgs( "%s\n", IO_GetAvailableFunctionCommands( (IOFunctionCommands_t)i ) ), pPlayer );
			return TRUE;
		}
		else
		{
			UTIL_PrintConsole( "Available commands:\n", pPlayer );
			UTIL_PrintConsole( "scall func - Print all available functions\n", pPlayer );
			UTIL_PrintConsole( "scall cmds - Print all available commands\n", pPlayer );
		}
		return TRUE;
	}
#endif

	if ( m_pGameMode->IsTestModeActive() )
	{
		if (FStrEq(pcmd, "dev_spec"))
		{
			pPlayer->RemoveAllItems( FALSE );
			ChangePlayerTeam(pPlayer, ZP::Teams[ZP::TEAM_OBSERVER], FALSE, FALSE);
			SendPlayerTeamInfo(pPlayer);
			pPlayer->StartObserver();
			return TRUE;
		}
		else if (FStrEq(pcmd, "dev_zombie"))
		{
			pPlayer->RemoveAllItems( FALSE );
			ChangePlayerTeam(pPlayer, ZP::Teams[ZP::TEAM_ZOMBIE], FALSE, FALSE);
			SendPlayerTeamInfo(pPlayer);
			if ( pPlayer->IsObserver() )
				pPlayer->StopObserver();
			else
				pPlayer->StopWelcomeCam();
			// Give the player their arms.
			if ( sv_fafo_only.GetBool() )
				OnWeaponGive( pPlayer, "weapon_fafo" );
			else
				OnWeaponGive( pPlayer, "weapon_swipe" );
			return TRUE;
		}
		else if (FStrEq(pcmd, "dev_human"))
		{
			pPlayer->RemoveAllItems( FALSE );
			ChangePlayerTeam(pPlayer, ZP::Teams[ZP::TEAM_SURVIVIOR], FALSE, FALSE);
			SendPlayerTeamInfo(pPlayer);
			if ( pPlayer->IsObserver() )
				pPlayer->StopObserver();
			else
				pPlayer->StopWelcomeCam();
			// No longer a late joiner since we forced to spawn as a human.
			pPlayer->m_bPunishLateJoiner = false;
			// Give the player their default weapons.
			m_pGameMode->GiveWeapons( pPlayer );
			return TRUE;
		}
		else if (FStrEq(pcmd, "dev_nolives"))
		{
			// Toggle the no lives.
			pPlayer->m_bNoLives = !pPlayer->m_bNoLives;
			return TRUE;
		}
		else if (FStrEq(pcmd, "test_ammoid"))
		{
			for ( int i = 0; i < ZPAmmoTypes::AMMO_MAX; i++ )
			{
				AmmoData ammo = GetAmmoByAmmoID( i );
				if ( !ammo.AmmoName ) continue;
				UTIL_SayText(CHAT_FILTER_SERVERMSG, UTIL_VarArgs("%s ID: [%i]", ammo.AmmoName, ammo.AmmoType), pPlayer);
			}
			return TRUE;
		}
		// Round related
		else if (FStrEq(pcmd, "dev_resetround"))
		{
			m_pGameMode->SetWinState( IGameModeBase::WinState_e::State_Draw );
			m_pGameMode->SetRoundState( ZP::RoundState::RoundState_RoundIsOver );
			return TRUE;
		}
		else if (FStrEq(pcmd, "dev_roundend"))
		{
			const char *pSetCommand = CMD_ARGV(1);
			int nValue = 0;
			if ( pSetCommand && pSetCommand[0] )
				nValue = atoi( pSetCommand );
			switch ( nValue )
			{
				case 1: m_pGameMode->SetWinState( IGameModeBase::WinState_e::State_SurvivorWin ); break;
				case 2: m_pGameMode->SetWinState( IGameModeBase::WinState_e::State_Draw ); break;
				default: m_pGameMode->SetWinState( IGameModeBase::WinState_e::State_ZombieWin ); break;
			}
			return TRUE;
		}
		else if (FStrEq(pcmd, "dev_killach"))
		{
			const char *arg1 = CMD_ARGV(1);
			const char *arg2 = CMD_ARGV(2);
			if ( FStrEq( arg2, "1" ) )
				pPlayer->m_iDeathFlags |= PLR_DEATH_FLAG_HEADSHOT;
			else
				pPlayer->m_iDeathFlags &= ~PLR_DEATH_FLAG_HEADSHOT;
			pPlayer->GiveAchievementsFromKill( pPlayer, arg1 );
			return TRUE;
		}
	}
	else
	{
		// If we are not in test mode, but someone tries to use dev_ related commands,
		// tell them that test mode is not active.
		if ( V_strnicmp( pcmd, "dev_", 4 ) == 0 )
		{
			UTIL_PrintConsole( "sv_testmode is not active. dev_ related commands are disabled.\n", pPlayer );
			return TRUE;
		}
	}
	return BaseClass::ClientCommand(pPlayer, pcmd);
}

void CZombiePanicGameRules::OnWeaponGive( CBasePlayer *pPlayer, const char *szItem )
{
	bool bIsCheatsEnabled = CVAR_GET_FLOAT("sv_cheats") >= 1 ? true : false;
	if ( !bIsCheatsEnabled ) return;

	bool bValid = false;
	if ( szItem && szItem[0] )
		bValid = true;

	if ( !bValid )
	{
		std::vector<WeaponInfo> found;

		// Iterate all weapons
		{
			for ( size_t i = 0; i < LAST_WEAPON_ID; i++ )
			{
				WeaponInfo weapon = GetWeaponInfo( (ZPWeaponID)i );
				if ( !weapon.szWeapon ) continue;
				if ( weapon.Hidden ) continue;
				found.push_back( weapon );
			}
		}

		// Sort array
		qsort(found.data(), found.size(), sizeof(WeaponInfo), [](const void *i, const void *j) -> int
		{
			const WeaponInfo *lhs = (const WeaponInfo *)i;
			const WeaponInfo *rhs = (const WeaponInfo *)j;
			return strcmp(lhs->szWeapon, rhs->szWeapon);
		});

		// Display results
		UTIL_PrintConsole( "Available weapons:\n", pPlayer );
		for (WeaponInfo& i : found)
			UTIL_PrintConsole( UTIL_VarArgs( "%s\n", i.szWeapon ), pPlayer );
		UTIL_PrintConsole( "--------------------\n", pPlayer );
		UTIL_PrintConsole( "Usage: give <string>\n", pPlayer );
		return;
	}

	pPlayer->GiveNamedItem( szItem );
}

extern void RemovePlayerLastSpawnPointData( CBasePlayer *pPlayer );

bool CZombiePanicGameRules::DownloadMissingWorkshopItem( edict_t *pClient )
{
	if ( g_ulCurrentWorkshopID == 0 ) return false;
	// Download the required Workshop item automatically (if the client does not have said item)
	CLIENT_COMMAND( pClient, "cl_workshop_download %llu %i\n", g_ulCurrentWorkshopID, m_bHostHasJoined );
	// Our first client that connects, is ALWAYS the host. So make sure we set this to true.
	if ( !m_bHostHasJoined ) m_bHostHasJoined = true;
	return true;
}

BOOL CZombiePanicGameRules::ClientConnected(edict_t *pClient, const char *pszName, const char *pszAddress, char szRejectReason[128])
{
	DownloadMissingWorkshopItem( pClient );
	return BaseClass::ClientConnected( pClient, pszName, pszAddress, szRejectReason );
}

void CZombiePanicGameRules::ClientDisconnected(edict_t *pClient)
{
	CBasePlayer *pPlayer = (CBasePlayer *)CBaseEntity::Instance(pClient);
	if ( pPlayer )
	{
		RemovePlayerLastSpawnPointData( pPlayer );
		m_pGameMode->OnPlayerDisconnected( pPlayer );

		// Reset on disconnect
		ClientAPIData_t data = m_ClientsData[ pPlayer->entindex() ] = {};
		MESSAGE_BEGIN( MSG_ALL, gmsgZPAPICall );
		WRITE_SHORT( pPlayer->entindex() );
		WRITE_SHORT( data.Game );
		WRITE_SHORT( data.Tier );
		WRITE_STRING( data.Key.c_str() );
		MESSAGE_END();
	}

	BaseClass::ClientDisconnected( pClient );
}

bool CZombiePanicGameRules::IsTestModeActive() const
{
	return m_pGameMode->IsTestModeActive();
}

bool CZombiePanicGameRules::WasCheatsOnThisSession() const
{
	return m_bCheatsOnThisSession;
}

void CZombiePanicGameRules::EndMultiplayerGame( void )
{
	// No gamemode? Stop.
	if ( !m_pGameMode ) return;
	// Already won/lost? Stop.
	if ( m_pGameMode->GetWinState() > IGameModeBase::WinState_e::State_None ) return;
	IGameModeBase::WinState_e nState = IGameModeBase::WinState_e::State_Draw;
	// Grab the new win state from our current gamemode type.
	ZP::GameModeType_e nGameMode = m_pGameMode->GetGameModeType();
	switch ( nGameMode )
	{
		case ZP::GameModeType_e::GAMEMODE_SURVIVAL:
		case ZP::GameModeType_e::GAMEMODE_HARDCORE: nState = IGameModeBase::WinState_e::State_SurvivorWin; break;
		case ZP::GameModeType_e::GAMEMODE_OBJECTIVE: nState = IGameModeBase::WinState_e::State_ZombieWin; break;
	}
	// Set our win state.
	m_pGameMode->SetWinState( nState );
}

/**
 * Parses string into a SteamID64. Returns 0 if failed.
 * Credits to voogru
 * https://forums.alliedmods.net/showthread.php?t=60899?t=60899
 * --------------------------------
 * Copied from the client side.
 */
uint64 UTIL_ParseSteamID( const char *pszAuthID )
{
	if ( !pszAuthID ) return 0;

	char steamid[ MAX_STEAMID ];
	if ( !strncmp( pszAuthID, "STEAM_", 6 ) || !strncmp( pszAuthID, "VALVE_", 6 ) )
		strncpy( steamid, pszAuthID + 6, MAX_STEAMID - 1 ); // cutout "STEAM_" or "VALVE_" start of the string
	else
		strncpy( steamid, pszAuthID, MAX_STEAMID - 1 );

	steamid[ MAX_STEAMID - 1 ] = '\0';

	// Valid SteamID must begin with 0:Y:ZZZZZZZ
	// "The value of X (Universe) is 0 in VALVe's GoldSrc and Source Orange Box Engine games"
	// https://developer.valvesoftware.com/wiki/SteamID
	if ( strncmp( steamid, "0:", 2 ) )
		return 0;

	int iServer = 0;
	int iAuthID = 0;

	char szAuthID[64];
	strncpy( szAuthID, steamid, sizeof(szAuthID) - 1 );
	szAuthID[sizeof(szAuthID) - 1] = '\0';

	char *szTmp = strtok(szAuthID, ":");
	while (szTmp = strtok(NULL, ":"))
	{
		char *szTmp2 = strtok(NULL, ":");
		if (szTmp2)
		{
			iServer = atoi(szTmp);
			iAuthID = atoi(szTmp2);
		}
	}

	if ( iAuthID == 0 )
		return 0;

	uint64 i64friendID = (long long)iAuthID * 2;

	//Friend ID's with even numbers are the 0 auth server.
	//Friend ID's with odd numbers are the 1 auth server.
	i64friendID += 76561197960265728 + iServer;

	return i64friendID;
}

void CZombiePanicGameRules::DoAPICallBack( CBasePlayer *pPlayer )
{
	m_vecPendingAPICalls.push_back( pPlayer->entindex() );
}

ClientAPIData_t CZombiePanicGameRules::GetClientAPI( CBasePlayer *pPlayer )
{
	if ( !pPlayer )
		return ClientAPIData_t();
	return m_ClientsData[ pPlayer->entindex() ];
}

void CZombiePanicGameRules::ProcessAPICalls()
{
	if ( m_vecPendingAPICalls.size() == 0 ) return;
	// Don't call it right away, we don't want to overload the clients with too many messages at once.
	if ( m_flNextAPICallTime - gpGlobals->time > 0 ) return;
	int index = m_vecPendingAPICalls[0];
	ClientAPIData_t data = m_ClientsData[ index ];

	// Erase after use.
	m_vecPendingAPICalls.erase( m_vecPendingAPICalls.begin() );

	// Send the info to the clients.
	MESSAGE_BEGIN( MSG_ALL, gmsgZPAPICall );
	WRITE_SHORT( index );
	WRITE_SHORT( data.Game );
	WRITE_SHORT( data.Tier );
	WRITE_STRING( data.Key.c_str() );
	MESSAGE_END();

	// Wait until we can process the next one. Only useful if we just changed level.
	m_flNextAPICallTime = gpGlobals->time + 0.8f; // Process every 0.2 seconds
}

void CZombiePanicGameRules::CheckCheats()
{
	if ( CVAR_GET_FLOAT("sv_cheats") >= 1 )
		m_bCheatsOnThisSession = true;
	// Make sure to also check for cheat vars.
	CvarSystem::CheckForCheatVars();

	// Simply check for specific cvars and make sure they are on their correct values for Zombie Panic!
	if ( !sv_disable_zpoverrides.GetBool() )
	{
#define CHECK_CVAR( cvar, def_value ) \
		float fl##cvar = CVAR_GET_FLOAT( #cvar ); \
		if ( fl##cvar != def_value ) CVAR_SET_FLOAT( #cvar, def_value )

		CHECK_CVAR( sv_graity, 800 );
		CHECK_CVAR( sv_airaccelerate, 10 );
		CHECK_CVAR( pausable, 0 ); // Never let em pause the game.
	}
}

void CZombiePanicGameRules::SetPlayerModel(CBasePlayer *pPlayer)
{
	// Moved to this function instead.
	pPlayer->SetTheCorrectPlayerModel();
}

//=========================================================
// ClientUserInfoChanged
//=========================================================
void CZombiePanicGameRules::ClientUserInfoChanged(CBasePlayer *pPlayer, char *infobuffer)
{
}

extern int gmsgDeathMsg;

//=========================================================
// Deathnotice.
//=========================================================
void CZombiePanicGameRules::DeathNotice(CBasePlayer *pVictim, entvars_t *pKiller, entvars_t *pevInflictor)
{
	if (m_DisableDeathMessages)
		return;

	if (pVictim && pKiller && pKiller->flags & FL_CLIENT)
	{
		CBasePlayer *pk = (CBasePlayer *)CBaseEntity::Instance(pKiller);
		if (pk && (pk != pVictim))
		{
			if ( (PlayerRelationship(pVictim, pk) == GR_TEAMMATE) )
				pVictim->m_iDeathFlags |= PLR_DEATH_FLAG_TEAMKILLER;
			else if ( pVictim->pev->team == ZP::TEAM_SURVIVIOR && pk->pev->team == ZP::TEAM_ZOMBIE )
			{
				int iPlayers = 0;
				for ( int i = 1; i <= gpGlobals->maxClients; i++ )
				{
					CBasePlayer *plr = (CBasePlayer *)UTIL_PlayerByIndex( i );
					if ( plr && plr->IsConnected() && plr->pev->team == ZP::TEAM_SURVIVIOR )
						iPlayers++;
				}
				if ( iPlayers == 1 )
					pk->GiveAchievement( EAchievements::ZOMBIEDESSERT );
			}
		}
	}

	BaseClass::DeathNotice(pVictim, pKiller, pevInflictor);
}

//=========================================================
//=========================================================
void CZombiePanicGameRules ::PlayerKilled(CBasePlayer *pVictim, entvars_t *pKiller, entvars_t *pInflictor)
{
	if (!m_DisableDeathPenalty)
	{
		m_pGameMode->OnPlayerDied( pVictim, pKiller, pInflictor );
		BaseClass::PlayerKilled(pVictim, pKiller, pInflictor);

		CBasePlayer *pk = (CBasePlayer *)CBaseEntity::Instance(pKiller);
		if (pk && (pk != pVictim))
			pVictim->GiveAchievement( EAchievements::FIRST_TO_DIE );
	}
}

void CZombiePanicGameRules::PlayerThink(CBasePlayer *pPlayer)
{
	BaseClass::PlayerThink( pPlayer );
}

//=========================================================
// IsTeamplay
//=========================================================
BOOL CZombiePanicGameRules::IsTeamplay(void)
{
	return TRUE;
}

BOOL CZombiePanicGameRules::FPlayerCanTakeDamage(CBasePlayer *pPlayer, CBaseEntity *pInflictor, CBaseEntity *pAttacker)
{
	// TEAM_NONE and TEAM_OBSERVER will never be damaged.
	if ( pPlayer->pev->team <= ZP::TEAM_OBSERVER ) return FALSE;

	// If valid attacker
	if ( pAttacker )
	{
		// We got ourselves, always return true.
		if ( pAttacker == pPlayer ) return TRUE;

		// If the attack was from an explosive,
		// and the one who threw it is dead,
		// don't cause any dmg to other survivors.
		if ( pInflictor && pInflictor->pev->team == ZP::TEAM_SURVIVIOR )
		{
			// If this is true, if we are a zombie player, always return true.
			bool bThrowerIsDeadOrNotSurvivor = !pAttacker->IsAlive();
			if ( pAttacker->pev->team != ZP::TEAM_SURVIVIOR )
				bThrowerIsDeadOrNotSurvivor = true;
			if ( bThrowerIsDeadOrNotSurvivor )
			{
				if ( pPlayer->pev->team == ZP::TEAM_SURVIVIOR ) return FALSE;
				return TRUE;
			}
		}

		// Player relationship stuff
		if ( PlayerRelationship(pPlayer, pAttacker) == GR_TEAMMATE )
		{
			// If friendly fire is off, and this hit came from someone other than myself, then don't get hurt.
			if ((friendlyfire.value == 0))
				return FALSE;
		}
	}

	// True by default.
	return TRUE;
}

//=========================================================
//=========================================================
int CZombiePanicGameRules::PlayerRelationship(CBaseEntity *pPlayer, CBaseEntity *pTarget)
{
	// half life multiplay has a simple concept of Player Relationships.
	// you are either on another player's team, or you are not.
	if (!pPlayer || !pTarget || !pPlayer->IsPlayer() || !pTarget->IsPlayer())
		return GR_NOTTEAMMATE;
	// Spectators are teammates, but not players in welcomecam mode
	if (((CBasePlayer *)pPlayer)->IsObserver() && !((CBasePlayer *)pPlayer)->m_bInWelcomeCam && ((CBasePlayer *)pTarget)->IsObserver() && !((CBasePlayer *)pTarget)->m_bInWelcomeCam)
		return GR_TEAMMATE;

	if ((*GetTeamID(pPlayer) != '\0') && (*GetTeamID(pTarget) != '\0') && !_stricmp(GetTeamID(pPlayer), GetTeamID(pTarget)))
	{
		return GR_TEAMMATE;
	}

	return GR_NOTTEAMMATE;
}

//=========================================================
//=========================================================
BOOL CZombiePanicGameRules::ShouldAutoAim(CBasePlayer *pPlayer, edict_t *target)
{
	// always autoaim, unless target is a teammate
	CBaseEntity *pTgt = CBaseEntity::Instance(target);
	if (pTgt && pTgt->IsPlayer())
	{
		if (PlayerRelationship(pPlayer, pTgt) == GR_TEAMMATE)
			return FALSE; // don't autoaim at teammates
	}

	return BaseClass::ShouldAutoAim(pPlayer, target);
}

//=========================================================
//=========================================================
int CZombiePanicGameRules::IPointsForKill(CBasePlayer *pAttacker, CBasePlayer *pKilled)
{
	if (!pKilled)
		return 0;

	if (!pAttacker)
		return 1;

	if (pAttacker != pKilled && PlayerRelationship(pAttacker, pKilled) == GR_TEAMMATE)
		return -1;

	return 1;
}

//=========================================================
//=========================================================
const char *CZombiePanicGameRules::GetTeamID(CBaseEntity *pEntity)
{
	if (pEntity == NULL || pEntity->pev == NULL)
		return "";

	// return their team name
	return pEntity->TeamID();
}

int CZombiePanicGameRules::GetTeamIndex(const char *pTeamName)
{
	if (pTeamName && *pTeamName != 0)
	{
		// try to find existing team
		for (int tm = 0; tm < ZP::MAX_TEAM; tm++)
		{
			if (!_stricmp(ZP::Teams[tm], pTeamName))
				return tm;
		}
	}

	return -1; // No match
}

const char *CZombiePanicGameRules::GetIndexedTeamName(int teamIndex)
{
	if (teamIndex < 0 || teamIndex >= ZP::MAX_TEAM)
		return ZP::Teams[0];
	return ZP::Teams[teamIndex];
}

BOOL CZombiePanicGameRules::IsValidTeam(const char *pTeamName)
{
	return (GetTeamIndex(pTeamName) != -1) ? TRUE : FALSE;
}

CZombiePanicGameRules *ZPGameRules()
{
	return dynamic_cast< CZombiePanicGameRules *>( g_pGameRules );
}
