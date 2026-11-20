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

// What GearDialog::EndModal does with the mount's calibration (#139)
enum class GearCalibrationAction
{
    None,
    AutoLoad, // load the calibration saved in the profile
    Clear, // the camera changed: the calibration no longer fits it
};

// What a gear session saw of the camera, for GearDialog::EndModal (#139). A session is one run of the Connect Equipment
// dialog, or one gear request (set_connected, set_profile) in the headless daemon, which never runs the dialog.
// - The image-scale change of the session's camera connects is taken once, by the EndModal that acts on it
//   (calibration step, MinMove, Alert): a later request, or the second EndModal of a connect all, must not redo it
//   over what the client has set since. The camera-change warning is given once per session.
// - A camera change outlives a headless request, because it is the mount's calibration it invalidates and the mount
//   may connect in a later request (guide's connect, the #104 retry, a per-device set_connected): the calibration is
//   cleared once, by the first EndModal with the mount connected, and the saved one is never auto-loaded again in
//   this process, since it was made with the old camera. A run of the dialog starts it over (PHD2 behaviour).
struct CameraChangeLatch
{
    double scaleRatio = 1.0;
    bool camChanged = false; // the camera or image scale changed: no calibration auto-load
    bool clearPending = false; // ... and the mount's calibration is still to be cleared for it
    bool warningIssued = false;

    // a run of the Connect Equipment dialog: everything starts over
    void Begin() { *this = CameraChangeLatch(); }
    // a headless gear request: a camera change, and its calibration clear if the mount has not had it yet, carry over
    void BeginRequest()
    {
        scaleRatio = 1.0;
        warningIssued = false;
    }
    // a camera connect read the driver's pixel size; profilePixelSize is 0 when the profile had none
    void NotePixelSize(double profilePixelSize, double devicePixelSize);
    // the image scale changed by 1% or more
    bool ScaleChanged() const;
    // ScaleChanged(), and the change is taken: false from then on until a connect changes the scale again
    bool TakeScaleChange();
    // a camera connect found another camera or another image scale
    void NoteCameraChange()
    {
        camChanged = true;
        clearPending = true;
    }
    // the user cancelled the connect at the camera-change warning (dialog only)
    void CancelCameraChange()
    {
        camChanged = false;
        clearPending = false;
    }
    // what EndModal does with the mount's calibration; a Clear is taken. autoLoad: the profile's auto-load setting is on
    // (and an AO, if any, is connected); mountConnected: the mount is connected
    GearCalibrationAction TakeCalibrationAction(bool autoLoad, bool mountConnected);
};

// whether a gear request begins a session of the latch: only the outermost request (one nested in it, from a nested
// event loop, is part of it), and not while the Connect Equipment dialog runs its own session, so a request it refuses
// does not wipe what the dialog saw (#139)
bool BeginsGearRequest(int connectDepth, bool dialogOpen);

#endif
