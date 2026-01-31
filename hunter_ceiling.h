#pragma once
#include "cbase.h"
#include "in_buttons.h"

class CHunter : public CBasePlayer {
public:
    DECLARE_CLASS(CHunter, CBasePlayer);

    void CeilingThink();
    void TryCeilingLatch();
    void ExitCeiling();
    void DoCeilingPounce();

private:
    bool  m_bOnCeiling = false;
    float m_flCeilingEnterTime = 0.0f;
    Vector m_vCeilingNormal = vec3_origin;

    // config (ajusta como quieras o pon en cvars)
    float m_flMaxCeilingDist = 64.0f;
    float m_flPounceSpeed = 1400.0f;
    float m_flMinCeilingDot = 0.7f;
};
