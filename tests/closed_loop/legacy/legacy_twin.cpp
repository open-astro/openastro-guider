#include "legacy_twin.h"

#include "scope_calibration_legacy.h"

namespace closed_loop
{

namespace
{
    // Points the recorders at this twin for the duration of one call
    class Bind
    {
    public:
        Bind(legacy::TwinContext *ctx, int frame) : m_prev(legacy::g_twin)
        {
            legacy::g_twin = ctx;
            ctx->frame = frame;
        }
        ~Bind() { legacy::g_twin = m_prev; }
        Bind(const Bind&) = delete;
        Bind& operator=(const Bind&) = delete;

    private:
        legacy::TwinContext *m_prev;
    };

    PHD_Point ToPoint(const Pt& p)
    {
        PHD_Point r;
        if (p.valid)
            r.SetXY(p.x, p.y);
        return r;
    }
} // namespace

LegacyTwinRun::LegacyTwinRun(SimMount& sim) : m_ctx(new legacy::TwinContext), m_scope(new legacy::Scope)
{
    m_ctx->sim = &sim;
}

LegacyTwinRun::~LegacyTwinRun() = default;

void LegacyTwinRun::Apply(const Settings& s)
{
    m_scope->m_calibrationDuration = s.stepMs;
    m_scope->m_calibrationDistance = s.distancePx;
    m_scope->m_maxRaDuration = s.maxRaMs;
    m_scope->m_maxDecDuration = s.maxDecMs;
    m_scope->m_decGuideMode = (legacy::DEC_GUIDE_MODE) s.decMode;
    m_scope->m_assumeOrthogonal = s.assumeOrthogonal;
    m_ctx->fastRecenter = s.fastRecenter;
    m_ctx->maxMovePx = s.maxMovePx;
}

void LegacyTwinRun::Begin(const Pt& lock, const Settings& settings)
{
    Bind bind(m_ctx.get(), -1);
    Apply(settings);
    m_scope->BeginCalibration(ToPoint(lock));
}

void LegacyTwinRun::Clear()
{
    Bind bind(m_ctx.get(), -1);
    m_scope->ClearCalibration();
}

FrameReport LegacyTwinRun::OnFrame(const Pt& star, const Settings& settings)
{
    Bind bind(m_ctx.get(), m_frame++);
    Apply(settings);
    FrameReport r;
    r.error = m_scope->UpdateCalibrationState(ToPoint(star));
    r.calibrated = !r.error && m_scope->IsCalibrated();
    return r;
}

Measured LegacyTwinRun::Result() const
{
    const legacy::Calibration& c = m_scope->m_calibration;
    const legacy::CalibrationDetails& d = m_scope->m_calibrationDetails;
    Measured m;
    m.xAngle = c.xAngle;
    m.xRate = c.xRate;
    m.yAngle = c.yAngle;
    m.yRate = c.yRate;
    m.raParity = (Parity) c.raGuideParity;
    m.decParity = (Parity) c.decGuideParity;
    for (const legacy::wxRealPoint& p : d.raSteps)
        m.raSteps.emplace_back(p.x, p.y);
    for (const legacy::wxRealPoint& p : d.decSteps)
        m.decSteps.emplace_back(p.x, p.y);
    m.raStepCount = m_scope->m_raSteps;
    m.decStepCount = m_scope->m_decSteps;
    return m;
}

const std::vector<Event>& LegacyTwinRun::Events() const
{
    return m_ctx->events;
}

} // namespace closed_loop
