/*
 *  guide_algorithm_kernels.cpp
 *  PHD Guiding
 *
 *  Created by Bret McKee
 *  Copyright (c) 2012 Bret McKee
 *  All rights reserved.
 *
 *  Based upon work by Craig Stark.
 *  Copyright (c) 2006-2010 Craig Stark.
 *  All rights reserved.
 *
 *  The math of GuideAlgorithmHysteresis::result() and GuideAlgorithmResistSwitch::result(),
 *  moved here unchanged (#142).
 *
 *  This source code is distributed under the following "BSD" license
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *    Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *    Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *    Neither the name of Bret McKee, Dad Dog Development,
 *     Craig Stark, Stark Labs nor the names of its
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

#include "guide_algorithm_kernels.h"

#include <cmath>
#include <cstdlib>

namespace guide_algorithm_kernels
{

double HysteresisResult(HysteresisState& state, const HysteresisParams& params, double input)
{
    double dReturn = (1.0 - params.hysteresis) * input + params.hysteresis * state.lastMove;

    dReturn *= params.aggression;

    if (std::fabs(input) < params.minMove)
    {
        dReturn = 0.0;
    }

    state.lastMove = dReturn;

    return dReturn;
}

void ResistSwitchReset(ResistSwitchState& state)
{
    state.history.fill(0.0);
    state.currentSide = 0;
}

int ResistSwitchSign(double x)
{
    int iReturn = 0;

    if (x > 0.0)
    {
        iReturn = 1;
    }
    else if (x < 0.0)
    {
        iReturn = -1;
    }

    return iReturn;
}

namespace
{
    constexpr unsigned int HISTORY_SIZE = RESIST_SWITCH_HISTORY_SIZE;

    // The try block of GuideAlgorithmResistSwitch::result(): each throw is a return of its veto
    ResistSwitchVeto Decide(ResistSwitchState& state, const ResistSwitchParams& params, double input, ResistSwitchStep& step)
    {
        auto& history = state.history;

        if (std::fabs(input) < params.minMove)
        {
            return ResistSwitchVeto::BelowMinMove;
        }

        if (params.fastSwitchEnabled)
        {
            double thresh = 3.0 * params.minMove;
            if (ResistSwitchSign(input) != state.currentSide && std::fabs(input) > thresh)
            {
                step.fastSwitched = true;
                step.thresh = thresh;
                step.fastSwitchFrom = state.currentSide;
                step.fastSwitchTo = ResistSwitchSign(input);
                // force switch
                state.currentSide = 0;
                unsigned int i;
                for (i = 0; i < HISTORY_SIZE - 3; i++)
                    history[i] = 0.0;
                for (; i < HISTORY_SIZE; i++)
                    history[i] = input;
            }
        }

        int decHistory = 0;

        for (unsigned int i = 0; i < HISTORY_SIZE; i++)
        {
            if (std::fabs(history[i]) > params.minMove)
            {
                decHistory += ResistSwitchSign(history[i]);
            }
        }

        if (state.currentSide == 0 || ResistSwitchSign(state.currentSide) == -ResistSwitchSign(decHistory))
        {
            if (std::abs(decHistory) < 3)
            {
                return ResistSwitchVeto::NotCompelling;
            }

            double oldest = 0.0;
            double newest = 0.0;

            for (unsigned int i = 0; i < 3; i++)
            {
                oldest += history[i];
                newest += history[HISTORY_SIZE - (i + 1)];
            }

            if (std::fabs(newest) <= std::fabs(oldest))
            {
                return ResistSwitchVeto::NotGettingWorse;
            }

            step.switched = true;
            step.switchFrom = state.currentSide;
            step.switchTo = ResistSwitchSign(decHistory);
            step.decHistory = decHistory;
            step.oldest = oldest;
            step.newest = newest;

            state.currentSide = ResistSwitchSign(decHistory);
        }

        if (state.currentSide != ResistSwitchSign(input))
        {
            return ResistSwitchVeto::Overshoot;
        }

        return ResistSwitchVeto::None;
    }
} // namespace

ResistSwitchStep ResistSwitchResult(ResistSwitchState& state, const ResistSwitchParams& params, double input)
{
    ResistSwitchStep step;
    double rslt = input;

    // m_history.Add(input); m_history.RemoveAt(0);
    for (unsigned int i = 0; i + 1 < HISTORY_SIZE; i++)
        state.history[i] = state.history[i + 1];
    state.history[HISTORY_SIZE - 1] = input;

    step.veto = Decide(state, params, input, step);
    if (step.veto != ResistSwitchVeto::None)
    {
        rslt = 0.0;
    }

    rslt *= params.aggression;

    step.result = rslt;
    return step;
}

} // namespace guide_algorithm_kernels
