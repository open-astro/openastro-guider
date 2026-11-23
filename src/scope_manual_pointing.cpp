/*
 *  scope_manual_pointing.cpp
 *  PHD Guiding
 *
 *  Created by Andy Galasso.
 *  Copyright (c) 2016 openphdguiding.org
 *  All rights reserved.
 *
 *  This source code is distributed under the following "BSD" license
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *    Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *    Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *    Neither the name of openphdguiding.org nor the names of its
 *     contributors may be used to endorse or promote products derived from
 *     this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "phd.h"

wxString ScopeManualPointing::GetDisplayName()
{
    return _("Ask for coordinates");
}

Mount::MOVE_RESULT ScopeManualPointing::Guide(GUIDE_DIRECTION, int)
{
    return MOVE_ERROR;
}

bool ScopeManualPointing::Connect()
{
    m_latitude = pConfig->Profile.GetDouble("/scope/manual_pointing/latitude", 41.661612);
    m_longitude = pConfig->Profile.GetDouble("/scope/manual_pointing/longitude", -77.824979);
    m_ra = LST(m_longitude);
    m_dec = 0.0;
    m_sideOfPier = PierSide::PIER_SIDE_UNKNOWN;

    return Scope::Connect();
}

double ScopeManualPointing::GetDeclinationRadians()
{
    return IsConnected() ? m_dec : UNKNOWN_DECLINATION;
}

bool ScopeManualPointing::GetCoordinates(double *ra, double *dec, double *siderealTime)
{
    if (!IsConnected())
        return true; // error

    *ra = m_ra;
    *dec = degrees(m_dec);
    *siderealTime = LST(m_longitude);

    return false;
}

bool ScopeManualPointing::GetSiteLatLong(double *latitude, double *longitude)
{
    if (!IsConnected())
        return true; // error

    *latitude = m_latitude;
    *longitude = m_longitude;

    return false;
}

PierSide ScopeManualPointing::SideOfPier()
{
    return IsConnected() ? m_sideOfPier : PierSide::PIER_SIDE_UNKNOWN;
}

bool ScopeManualPointing::CanReportPosition()
{
    return true;
}
