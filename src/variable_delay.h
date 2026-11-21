/*
 *  variable_delay.h
 *  PHD Guiding
 *
 *  The seconds <-> milliseconds conversion of the variable exposure delay's
 *  JSON-RPC methods. get_ and set_variable_delay_settings speak seconds
 *  (ShortDelaySeconds, LongDelaySeconds); the delays are stored, and saved in
 *  the profile, in milliseconds (open-astro/openastro-guider#174). Also the
 *  delay a frame gets: MyFrame decides it on the main thread when it schedules
 *  the exposure and hands it to the worker thread with the request (#182).
 *  Kept free of wx so it can be unit tested (tests/event_server/test_variable_delay.cpp).
 */

#ifndef VARIABLE_DELAY_H_INCLUDED
#define VARIABLE_DELAY_H_INCLUDED

namespace VariableDelay
{
// The delay in ms that set_variable_delay_settings stores for a delay of `seconds`, rounded to the nearest ms.
int SecondsToMs(double seconds);

// The delay in seconds that get_variable_delay_settings reports for a stored delay of `ms`: whole seconds for a
// whole number of seconds, which the getter prints as a JSON integer, as PHD2 documents.
double MsToSeconds(int ms);

// What the next frame's delay depends on besides the settings. MyFrame reads it on the main thread, where it
// is written, when it schedules the frame's exposure (#182).
struct FrameState
{
    bool guiding = false; // Guider::IsGuiding()
    bool controllerIdle = false; // PhdController::IsIdle(): no settle or other guide / dither request in progress
    bool recentering = false; // Guider::IsRecentering(): a dither's fast recenter still has steps to send
    bool guidingEnabled = false; // Mount::GetGuidingEnabled()
};

// The delay in ms before the next frame: the time lapse when the variable delay is off; with it on, the long
// delay while guiding steadily (guiding, controller idle, not recentering, guide output enabled) and the short
// delay otherwise.
int ForNextFrame(bool enabled, int shortDelayMs, int longDelayMs, int timeLapseMs, const FrameState& state);
} // namespace VariableDelay

#endif
