/*
 *  connect_request.h
 *  PHD Guiding
 *
 *  set_connected / get_connected per device (#84): the device names and when
 *  a request is refused, and what a gear request saw of the camera (#139).
 *  Kept free of wx so it can be unit tested
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

// What one gear session saw of the camera, for GearDialog::EndModal (#139): the image-scale change of its camera
// connects, whether the camera changed, and whether the camera-change warning was given. A session is one run of the
// Connect Equipment dialog, or one gear request (set_connected, set_profile) in the headless daemon, which never runs
// the dialog; each begins afresh. A scale change is taken once, by the EndModal that acts on it (calibration step,
// MinMove, Alert): a later request, or the second EndModal of a connect all, must not redo it over what the client
// has set since.
struct CameraChangeLatch
{
    double scaleRatio = 1.0;
    bool camChanged = false;
    bool warningIssued = false;

    void Begin() { *this = CameraChangeLatch(); }
    // a camera connect read the driver's pixel size; profilePixelSize is 0 when the profile had none
    void NotePixelSize(double profilePixelSize, double devicePixelSize);
    // the image scale changed by 1% or more
    bool ScaleChanged() const;
    // ScaleChanged(), and the change is taken: false from then on until a connect changes the scale again
    bool TakeScaleChange();
};

#endif
