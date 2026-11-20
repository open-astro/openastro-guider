/*
 *  guider_state.h
 *  PHD Guiding
 *
 *  The guider's state machine states, moved out of guider.h so wx-free code can
 *  take a state (CaptureFailures::PhaseFor, #136).
 */

#ifndef GUIDER_STATE_H_INCLUDED
#define GUIDER_STATE_H_INCLUDED

enum GUIDER_STATE
{
    STATE_UNINITIALIZED = 0,
    STATE_SELECTING,
    STATE_SELECTED,
    STATE_CALIBRATING_PRIMARY,
    STATE_CALIBRATING_SECONDARY,
    STATE_CALIBRATED,
    STATE_GUIDING,
    STATE_STOP, // This is a pseudo state
};

#endif
