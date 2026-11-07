// ============== Copyright (c) 2025 Monochrome Games ============== \\


#include "extdll.h"
#include "util.h"
#include "eiface.h"
#ifdef SCRIPT_SYSTEM
#include "core.h"
#endif

#define SF_MULTIRELAY_RESET_ON_REACH (1 << 0) // Start disabled


class CMultiRelay : public CPointEntity
{
	SET_BASECLASS( CPointEntity );

public:
	void Spawn( void );
	void Restart();
	void KeyValue( KeyValueData *pkvd ) override;
	void Use( CBaseEntity *pActivator, CBaseEntity *pCaller, USE_TYPE useType, float value );
	virtual int ObjectCaps(void) { return CBaseEntity::ObjectCaps() | FCAP_MUST_RESET; }

private:
	int m_cTargets = 0; // the total number of targets in this manager's fire list.
	int m_index; // Current target
	int m_iCurrent; // Current fire amount
	int m_iTargetName[MAX_MULTI_TARGETS]; // list if indexes into global string array
	int m_iTargetFire[MAX_MULTI_TARGETS]; // how many fires it needs until we can fire this one
	int m_iTargetType[MAX_MULTI_TARGETS]; // What is our type? OFF, ON or TOGGLE?
};
LINK_ENTITY_TO_CLASS( multi_relay, CMultiRelay );


void CMultiRelay::KeyValue( KeyValueData *pkvd )
{
	// this assumes that additional fields are targetnames and their values are delay values.
	if ( m_cTargets < MAX_MULTI_TARGETS )
	{
		char tmp[128];

		UTIL_StripToken(pkvd->szKeyName, tmp, sizeof(tmp));
		m_iTargetName[m_cTargets] = ALLOC_STRING(tmp);

		// Check we find a comma, make sure we add in the m_flTargetType
		std::string str( pkvd->szValue );
		size_t nComma = str.find( ',' );
		if ( nComma != std::string::npos )
		{
			m_iTargetFire[m_cTargets] = atoi( str.substr( 0, nComma ).c_str() );
			m_iTargetType[m_cTargets] = atoi( str.substr( nComma + 1, str.size() - 1 ).c_str() );
		}
		else
		{
			m_iTargetFire[m_cTargets] = atoi( pkvd->szValue );
			m_iTargetType[m_cTargets] = USE_TOGGLE;
		}
		m_cTargets++;
		pkvd->fHandled = TRUE;
	}
}


void CMultiRelay::Spawn()
{
	BaseClass::Spawn();
	m_index = 0;
	m_iCurrent = 0;
	// Sort targets
	// Quick and dirty bubble sort
	int swapped = 1;

	while (swapped)
	{
		swapped = 0;
		for (int i = 1; i < m_cTargets; i++)
		{
			if (m_iTargetFire[i] < m_iTargetFire[i - 1])
			{
				// Swap out of order elements
				int name = m_iTargetName[i];
				int amt = m_iTargetFire[i];
				int type = m_iTargetType[i];
				m_iTargetName[i] = m_iTargetName[i - 1];
				m_iTargetFire[i] = m_iTargetFire[i - 1];
				m_iTargetType[i] = m_iTargetType[i - 1];
				m_iTargetName[i - 1] = name;
				m_iTargetFire[i - 1] = amt;
				m_iTargetType[i - 1] = type;
				swapped = 1;
			}
		}
	}
}


void CMultiRelay::Restart()
{
	m_index = 0;
	m_iCurrent = 0;
}


void CMultiRelay::Use( CBaseEntity *pActivator, CBaseEntity *pCaller, USE_TYPE useType, float value )
{
	if ( m_index >= m_cTargets )
	{
		if ( (pev->spawnflags & SF_MULTIRELAY_RESET_ON_REACH) )
			Restart();
		else
			return;
	}
	m_iCurrent++;
	if ( m_iTargetFire[m_index] > m_iCurrent ) return;
	CBaseEntity *pFind = UTIL_FindEntityByTargetname( nullptr, STRING(m_iTargetName[m_index]) );
	while ( pFind )
	{
		pFind->Use( pActivator, this, (USE_TYPE)m_iTargetType[m_index], 0 );
		pFind = UTIL_FindEntityByTargetname( pFind, STRING(m_iTargetName[m_index]) );
	}
	m_index++;
}
