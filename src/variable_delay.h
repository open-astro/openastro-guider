/*
 *  variable_delay.h
 *  PHD Guiding
 *
 *  The seconds <-> milliseconds conversion of the variable exposure delay's
 *  JSON-RPC methods. get_ and set_variable_delay_settings speak seconds
 *  (ShortDelaySeconds, LongDelaySeconds); the delays are stored, and saved in
 *  the profile, in milliseconds (open-astro/openastro-guider#174).
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
} // namespace VariableDelay

#endif
