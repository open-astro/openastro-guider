/*
 *  imaging_gear_alpaca.h
 *  PHD Guiding
 *
 *  Connects the imaging-gear poller (imaging_gear_poller.h) to the Alpaca
 *  server the guider uses, for the guide log and the get_imaging_gear /
 *  set_imaging_gear_devices RPCs. Main thread only.
 */

#ifndef IMAGING_GEAR_ALPACA_H_INCLUDED
#define IMAGING_GEAR_ALPACA_H_INCLUDED

#include "imaging_gear.h"

#include <string>
#include <vector>

namespace ImagingGearLog
{
// start/stop the background reader
void Start();
void Stop();

// tell the reader what the guider is connected to; call after equipment
// connects or disconnects
void EquipmentChanged();

// the header lines for a guiding or calibration section (discards INFO lines
// queued before it, since the header describes the current state)
std::string Header();

// INFO lines queued since the last call
std::vector<std::string> TakeInfoLines();

// The devices the imaging app named with set_imaging_gear_devices. Kept until
// changed or the guider exits (not saved in the profile); setting them makes
// the next poll rediscover the devices.
ImagingGear::Overrides Overrides();
void SetOverrides(const ImagingGear::Overrides& overrides);

// the guide camera's Alpaca device number, which is never the imaging camera
int GuideCamera();

// get_imaging_gear: the cached snapshot as JSON; never waits on the network
std::string Json();
}

#endif
