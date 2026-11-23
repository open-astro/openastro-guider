/*
 *  gear_simulator.cpp
 *  PHD Guiding
 *
 *  Created by Craig Stark.
 *  Reimplemented for PHD2 by Andy Galasso.
 *  Copyright (c) 2006-2010 Craig Stark
 *  Copyright (c) 2015-2018 Andy Galasso
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

#include "gear_simulator.h"

#ifdef STEPGUIDER_SIMULATOR

struct SimAoParams
{
    static unsigned int max_position; // max position in steps
    static double scale; // arcsec per step
    static double angle; // angle relative to camera (degrees)
};

unsigned int SimAoParams::max_position = 45;
double SimAoParams::scale = 0.10;
double SimAoParams::angle = 35.0;

class StepGuiderSimulator : public StepGuider
{
public:
    StepGuiderSimulator();
    virtual ~StepGuiderSimulator();

    bool Connect() override;
    bool Disconnect() override;

private:
    bool Center() override;
    STEP_RESULT Step(GUIDE_DIRECTION direction, int steps) override;
    int MaxPosition(GUIDE_DIRECTION direction) const override;
    bool SetMaxPosition(int steps) override;
    bool HasNonGuiMove() override;
};

static StepGuiderSimulator *s_sim_ao;

StepGuiderSimulator::StepGuiderSimulator()
{
    m_Name = _("AO-Simulator");
    SimAoParams::max_position = pConfig->Profile.GetInt("/SimAo/max_steps", 45);
}

StepGuiderSimulator::~StepGuiderSimulator() { }

bool StepGuiderSimulator::Connect()
{
    if (StepGuider::Connect())
        return true;

    ZeroCurrentPosition();

    s_sim_ao = this;

    Debug.AddLine("AO Simulator Connected");

    if (!pCamera || pCamera->Name != _T("Simulator"))
    {
        pFrame->Alert(_("The AO Simulator only works with the Camera Simulator. You should either disconnect the AO Simulator "
                        "or connect the Camera Simulator."));
    }

    return false;
}

bool StepGuiderSimulator::Disconnect()
{
    if (StepGuider::Disconnect())
        return true;

    if (s_sim_ao == this)
    {
        Debug.AddLine("AO Simulator Disconnected");
        s_sim_ao = 0;
    }

    return false;
}

bool StepGuiderSimulator::Center()
{
    ZeroCurrentPosition();
    return false;
}

StepGuider::STEP_RESULT StepGuiderSimulator::Step(GUIDE_DIRECTION direction, int steps)
{
# if 0 // enable to test step failure
    wxPoint pos = GetAoPos();
    if (direction == LEFT && pos.x - steps < -35)
    {
        Debug.Write("simulate step failure\n");
        return STEP_LIMIT_REACHED;
    }
# endif

    // parent class maintains x/y offsets, so nothing to do here. Just simulate a delay.
    enum
    {
        LATENCY_MS_PER_STEP = 5
    };
    wxMilliSleep(steps * LATENCY_MS_PER_STEP);
    return STEP_OK;
}

int StepGuiderSimulator::MaxPosition(GUIDE_DIRECTION direction) const
{
    return SimAoParams::max_position;
}

bool StepGuiderSimulator::SetMaxPosition(int steps)
{
    SimAoParams::max_position = (unsigned int) steps;
    pConfig->Profile.SetInt("/SimAo/max_steps", steps);
    return false;
}

bool StepGuiderSimulator::HasNonGuiMove()
{
    return true;
}

#endif // STEPGUIDER_SIMULATOR

#ifdef ROTATOR_SIMULATOR

class RotatorSimulator : public Rotator
{
public:
    RotatorSimulator();
    virtual ~RotatorSimulator();

    bool Connect() override;
    bool Disconnect() override;

    // get the display name of the rotator device
    wxString Name() const override;

    // get the rotator position in degrees
    float Position() const override;
};

RotatorSimulator::RotatorSimulator() { }

RotatorSimulator::~RotatorSimulator() { }

bool RotatorSimulator::Connect()
{
    if (!pCamera || pCamera->Name != _T("Simulator"))
    {
        pFrame->Alert(_("The Rotator Simulator only works with the Camera Simulator. You must either disconnect the Rotator "
                        "Simulator or connect the Camera Simulator."));
        return true;
    }

    Rotator::Connect();
    return false;
}

bool RotatorSimulator::Disconnect()
{
    Rotator::Disconnect();
    return false;
}

wxString RotatorSimulator::Name() const
{
    return _T("Simulator");
}

float RotatorSimulator::Position() const
{
    assert(IsConnected());
    // #115 S6.6: the camera simulator, which set the angle, is gone; the angle was 0 unless it had connected, and
    // this rotator connects only beside it
    return 0.f;
}

#endif // ROTATOR_SIMULATOR

StepGuider *GearSimulator::MakeAOSimulator()
{
#ifdef STEPGUIDER_SIMULATOR
    return new StepGuiderSimulator();
#else
    return nullptr;
#endif
}

Rotator *GearSimulator::MakeRotatorSimulator()
{
    return new RotatorSimulator();
}
