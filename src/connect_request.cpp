/*
 *  connect_request.cpp
 *  PHD Guiding
 *
 *  See connect_request.h.
 */

#include "connect_request.h"

#include <algorithm>
#include <cctype>
#include <cmath>

bool ParseGearDevice(const std::string& name, GearDevice *device)
{
    std::string lower(name);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char) std::tolower(c); });

    if (lower == "all")
        *device = GearDevice::All;
    else if (lower == "camera")
        *device = GearDevice::Camera;
    else if (lower == "mount")
        *device = GearDevice::Mount;
    else if (lower == "rotator")
        *device = GearDevice::Rotator;
    else
        return false;
    return true;
}

const char *GearDeviceName(GearDevice device)
{
    switch (device)
    {
    case GearDevice::Camera:
        return "camera";
    case GearDevice::Mount:
        return "mount";
    case GearDevice::Rotator:
        return "rotator";
    default:
        return "all";
    }
}

std::string ConnectRefusal(GearDevice device, bool connect, bool captureActive, bool calibratingOrGuiding)
{
    // these error messages are internal to the event server and are not translated
    switch (device)
    {
    case GearDevice::All:
        // same messages as GearDialog::ConnectAll / DisconnectAll
        if (captureActive)
            return connect ? "cannot connect equipment when capture is active"
                           : "cannot disconnect equipment while capture active";
        return "";

    case GearDevice::Camera:
        if (captureActive)
            return std::string("cannot ") + (connect ? "connect" : "disconnect") + " the camera while capture is active";
        return "";

    default:
        // the mount and rotator can join (or leave) while the camera loops,
        // but not under a calibration or guiding run
        if (calibratingOrGuiding)
            return std::string("cannot ") + (connect ? "connect" : "disconnect") + " the " + GearDeviceName(device) +
                " while calibrating or guiding";
        return "";
    }
}

void CameraChangeLatch::NotePixelSize(double profilePixelSize, double devicePixelSize)
{
    if (profilePixelSize > 0.0)
        scaleRatio *= devicePixelSize / profilePixelSize;
}

bool CameraChangeLatch::ScaleChanged() const
{
    return std::fabs(scaleRatio - 1.0) >= 0.01;
}

bool CameraChangeLatch::TakeScaleChange()
{
    bool changed = ScaleChanged();
    scaleRatio = 1.0;
    return changed;
}

GearCalibrationAction CameraChangeLatch::TakeCalibrationAction(bool autoLoad, bool mountConnected)
{
    if (!camChanged)
        return autoLoad && mountConnected ? GearCalibrationAction::AutoLoad : GearCalibrationAction::None;
    if (!clearPending || !mountConnected)
        return GearCalibrationAction::None;
    clearPending = false;
    return GearCalibrationAction::Clear;
}

bool BeginsGearRequest(int connectDepth)
{
    return connectDepth == 0;
}
