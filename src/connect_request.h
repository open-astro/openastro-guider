/*
 *  connect_request.h
 *  PHD Guiding
 *
 *  set_connected / get_connected per device (#84): the device names and when
 *  a request is refused. Kept free of wx so it can be unit tested
 *  (tests/equipment/test_connect_request.cpp).
 */

#ifndef CONNECT_REQUEST_H_INCLUDED
#define CONNECT_REQUEST_H_INCLUDED

#include <string>

enum class GearDevice
{
    All,
    Camera,
    Mount,
    Rotator,
};

// "all", "camera", "mount" or "rotator", any case
bool ParseGearDevice(const std::string& name, GearDevice *device);

const char *GearDeviceName(GearDevice device);

// why the request can't be done now, or "" when it can
std::string ConnectRefusal(GearDevice device, bool connect, bool captureActive, bool calibratingOrGuiding, bool dialogOpen);

#endif
