// ============== Copyright (c) 2025 Monochrome Games ============== \\

#ifndef SERVER_PROP_STATIC_SPAWN
#define SERVER_PROP_STATIC_SPAWN
#pragma once

class CPropStaticSpawn : public CPointEntity
{
	SET_BASECLASS( CPointEntity );
public:
	void Precache() override;
	void Spawn() override;
	void Restart() override;
	void SpawnItem();
	void KeyValue( KeyValueData *pkvd ) override;
	void Use( CBaseEntity *pActivator, CBaseEntity *pCaller, USE_TYPE useType, float value ) override;
	int ObjectCaps() override;
	bool IsUseableItem() const override;

protected:
	void SetSequenceBox();
	int ExtractBbox( int sequence, float *mins, float *maxs );

private:
	string_t m_GiveItem;
	int m_percentage;
	float m_flScale;
	bool m_bDisabled;
	bool m_bCanUse;
};

#endif
