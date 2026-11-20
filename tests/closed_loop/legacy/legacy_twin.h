// LegacyTwinRun: the harness's view of the frozen legacy twin (#142).
//
// It plays the parts of Scope and Guider around the copied code:
//   - Begin is Scope::BeginCalibration with the lock position;
//   - before every frame it writes the Settings into the twin's members and
//     the guider stand-in, as Scope reads them from its members and
//     pFrame->pGuider at the point of use;
//   - OnFrame is Scope::UpdateCalibrationState, then the IsCalibrated() test
//     Guider::UpdateGuideState makes after it;
//   - Clear is Scope::ClearCalibration (the legacy socket server's CLEARCAL).

#ifndef CLOSED_LOOP_LEGACY_TWIN_H
#define CLOSED_LOOP_LEGACY_TWIN_H

#include "../harness.h"

#include <memory>

namespace legacy
{
class Scope;
struct TwinContext;
} // namespace legacy

namespace closed_loop
{

class LegacyTwinRun
{
public:
    explicit LegacyTwinRun(SimMount& sim);
    ~LegacyTwinRun();
    LegacyTwinRun(const LegacyTwinRun&) = delete;
    LegacyTwinRun& operator=(const LegacyTwinRun&) = delete;

    void Begin(const Pt& lock, const Settings& settings);
    void Clear();
    FrameReport OnFrame(const Pt& star, const Settings& settings);
    Measured Result() const;
    const std::vector<Event>& Events() const;

private:
    void Apply(const Settings& settings);

    std::unique_ptr<legacy::TwinContext> m_ctx;
    std::unique_ptr<legacy::Scope> m_scope;
    int m_frame = 0;
};

} // namespace closed_loop

#endif // CLOSED_LOOP_LEGACY_TWIN_H
