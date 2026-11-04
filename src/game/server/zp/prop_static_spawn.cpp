// ============== Copyright (c) 2025 Monochrome Games ============== \\

#include "extdll.h"
#include "util.h"
#include "player.h"
#include "zp/prop_static_spawn.h"

#define SF_DISABLED (1<<0)

LINK_ENTITY_TO_CLASS( prop_static_spawn, CPropStaticSpawn );

void CPropStaticSpawn::Precache()
{
	BaseClass::Precache();
	PRECACHE_MODEL( (char*)STRING(pev->model) );
}

void CPropStaticSpawn::Spawn()
{
	Precache();
	BaseClass::Spawn();
	SET_MODEL( ENT(pev), (char*)STRING(pev->model) );
	if ( FBitSet( pev->spawnflags, SF_DISABLED ) )
		m_bDisabled = true;
	else
		m_bDisabled = false;
	m_bCanUse = false;

	pev->movetype = MOVETYPE_PUSHSTEP;
	pev->solid = SOLID_NOT;
	pev->effects = EF_NODRAW;
	pev->sequence = 0;
	pev->gravity = 0;
	SetSequenceBox();
}

void CPropStaticSpawn::SpawnItem()
{
	float flPercentage = (float)(m_percentage) / 100.0f;
	float flChance = RandomFloat( 0.0f, 100.0f ) / 100.0f;
	if ( flChance <= flPercentage || flPercentage == 1.0f )
	{
		pev->effects = 0;
		pev->scale = m_flScale;
		m_bCanUse = true;
	}
}

bool CPropStaticSpawn::IsUseableItem() const
{
	if ( m_bDisabled ) return false;
	return m_bCanUse;
}

void CPropStaticSpawn::Restart()
{
	if ( FBitSet( pev->spawnflags, SF_DISABLED ) )
		m_bDisabled = true;
	else
		m_bDisabled = false;
	m_bCanUse = false;
	pev->movetype = MOVETYPE_PUSHSTEP;
	pev->solid = SOLID_NOT;
	pev->effects = EF_NODRAW;
}

void CPropStaticSpawn::KeyValue( KeyValueData *pkvd )
{
	if ( FStrEq( pkvd->szKeyName, "model" ) )
		pev->model = ALLOC_STRING( pkvd->szValue );
	else if ( FStrEq( pkvd->szKeyName, "scale" ) )
		m_flScale = atof( pkvd->szValue );
	else if ( FStrEq( pkvd->szKeyName, "chance" ) )
		m_percentage = clamp( atoi( pkvd->szValue ), 0, 100 );
	else if ( FStrEq( pkvd->szKeyName, "give" ) )
		m_GiveItem = ALLOC_STRING( pkvd->szValue );
	else
		BaseClass::KeyValue( pkvd );
}

void CPropStaticSpawn::Use( CBaseEntity *pActivator, CBaseEntity *pCaller, USE_TYPE useType, float value )
{
	if ( pev->effects == EF_NODRAW ) return;

	// If a player uses this.
	if ( pActivator->IsPlayer() && useType == USE_SET )
	{
		if ( !pActivator->IsAlive() ) return;
		if ( pActivator->pev->team != ZP::TEAM_SURVIVIOR ) return;
		if ( m_bDisabled ) return;
		if ( !m_bCanUse ) return;
		m_bCanUse = false;
		CBasePlayer *pPlayer = dynamic_cast<CBasePlayer *>( pActivator );
		if ( pPlayer )
			pPlayer->GiveNamedItem( STRING(m_GiveItem) );
		SoftRemove();
	}

	// Toggle
	m_bDisabled = !m_bDisabled;
}

int CPropStaticSpawn::ObjectCaps()
{
	if ( pev->effects == EF_NODRAW ) return FCAP_MUST_RESET;
	return FCAP_MUST_RESET | FCAP_IMPULSE_USE;
}


//=========================================================
//=========================================================

extern int ExtractBbox( void *pmodel, int sequence, float *mins, float *maxs );

int CPropStaticSpawn::ExtractBbox( int sequence, float *mins, float *maxs )
{
	return ::ExtractBbox( GET_MODEL_PTR(ENT(pev)), sequence, mins, maxs );
}

void CPropStaticSpawn::SetSequenceBox()
{
	Vector mins, maxs;

	// Get sequence bbox
	if ( ExtractBbox( pev->sequence, mins, maxs ) )
	{
		// expand box for rotation
		// find min / max for rotations
		float yaw = pev->angles.y * (M_PI / 180.0);

		Vector xvector, yvector;
		xvector.x = cos(yaw);
		xvector.y = sin(yaw);
		yvector.x = -sin(yaw);
		yvector.y = cos(yaw);
		Vector bounds[2];

		bounds[0] = mins;
		bounds[1] = maxs;

		Vector rmin(9999, 9999, 9999);
		Vector rmax(-9999, -9999, -9999);
		Vector base, transformed;

		for (int i = 0; i <= 1; i++)
		{
			base.x = bounds[i].x;
			for (int j = 0; j <= 1; j++)
			{
				base.y = bounds[j].y;
				for (int k = 0; k <= 1; k++)
				{
					base.z = bounds[k].z;

					// transform the point
					transformed.x = xvector.x * base.x + yvector.x * base.y;
					transformed.y = xvector.y * base.x + yvector.y * base.y;
					transformed.z = base.z;

					if (transformed.x < rmin.x)
						rmin.x = transformed.x;
					if (transformed.x > rmax.x)
						rmax.x = transformed.x;
					if (transformed.y < rmin.y)
						rmin.y = transformed.y;
					if (transformed.y > rmax.y)
						rmax.y = transformed.y;
					if (transformed.z < rmin.z)
						rmin.z = transformed.z;
					if (transformed.z > rmax.z)
						rmax.z = transformed.z;
				}
			}
		}
		UTIL_SetSize(pev, rmin, rmax);
	}
}
