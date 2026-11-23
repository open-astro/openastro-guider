// A reader for PHD2 guide logs (#142 Tier 4). See guide_log.h.

#include "guide_log.h"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>

namespace guide_log
{

namespace
{

    bool StartsWith(const std::string& s, const char *prefix)
    {
        return s.rfind(prefix, 0) == 0;
    }

    // The number after "<key> = " in line
    bool Number(const std::string& line, const std::string& key, double *value)
    {
        size_t pos = line.find(key + " = ");
        if (pos == std::string::npos)
            return false;
        const char *start = line.c_str() + pos + key.size() + 3;
        char *end = nullptr;
        double v = std::strtod(start, &end);
        if (end == start)
            return false;
        *value = v;
        return true;
    }

    // The text after "<key> = " in line, up to a comma, a space or the end
    bool Word(const std::string& line, const std::string& key, std::string *value)
    {
        size_t pos = line.find(key + " = ");
        if (pos == std::string::npos)
            return false;
        pos += key.size() + 3;
        size_t end = line.find_first_of(", ", pos);
        *value = line.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        return true;
    }

    // "X guide algorithm = Hysteresis, Hysteresis = 0.100, Aggression = 0.700, Minimum move = 0.150"
    // "Y guide algorithm = Resist Switch, Minimum move = 0.150 Aggression = 70% FastSwitch = enabled"
    Algorithm ParseAlgorithm(const std::string& line)
    {
        Algorithm a;
        const std::string key = "guide algorithm = ";
        size_t pos = line.find(key) + key.size();
        size_t comma = line.find(',', pos);
        a.name = line.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        std::string rest = comma == std::string::npos ? std::string() : line.substr(comma);
        Number(rest, "Minimum move", &a.minMove);
        if (a.name == "Hysteresis")
        {
            Number(rest, "Hysteresis", &a.hysteresis);
            Number(rest, "Aggression", &a.aggression);
        }
        else if (a.name == "Resist Switch")
        {
            double pct;
            if (Number(rest, "Aggression", &pct))
                a.aggression = pct / 100.0;
            std::string fast;
            if (Word(rest, "FastSwitch", &fast))
                a.fastSwitch = fast == "enabled";
        }
        else if (a.name == "Lowpass")
            Number(rest, "Slope weight", &a.slopeWeight);
        else if (a.name == "Lowpass2")
            Number(rest, "Aggressiveness", &a.aggressiveness);
        return a;
    }

    std::vector<std::string> SplitCsv(const std::string& line)
    {
        std::vector<std::string> f;
        std::string item;
        std::stringstream ss(line);
        while (std::getline(ss, item, ','))
            f.push_back(item);
        if (!line.empty() && line.back() == ',')
            f.emplace_back();
        return f;
    }

    bool ToDouble(const std::string& s, double *v)
    {
        if (s.empty())
        {
            *v = 0.;
            return true;
        }
        char *end = nullptr;
        *v = std::strtod(s.c_str(), &end);
        return end != s.c_str() && *end == '\0';
    }

    bool ToInt(const std::string& s, int *v)
    {
        double d;
        if (!ToDouble(s, &d))
            return false;
        *v = (int) d;
        return true;
    }

    // frame,time,"Mount",dx,dy,RARaw,DECRaw,RAGuide,DECGuide,RADuration,RADirection,DECDuration,
    // DECDirection,XStep,YStep,StarMass,SNR,ErrorCode, or frame,time,"DROP",,,,,,,,,,,,,mass,SNR,
    // error,"status"
    bool ParseFrame(const std::string& line, int lineNo, Frame *f, std::string *error)
    {
        std::vector<std::string> v = SplitCsv(line);
        f->line = lineNo;
        bool ok = v.size() >= 18 && ToInt(v[0], &f->frame) && ToDouble(v[1], &f->time);
        if (ok && v[2] == "\"DROP\"")
        {
            f->dropped = true;
            ok = ToDouble(v[16], &f->snr) && ToInt(v[17], &f->errorCode);
        }
        else if (ok)
        {
            ok = ToDouble(v[3], &f->dx) && ToDouble(v[4], &f->dy) && ToDouble(v[5], &f->raRaw) && ToDouble(v[6], &f->decRaw) &&
                ToDouble(v[7], &f->raGuide) && ToDouble(v[8], &f->decGuide) && ToInt(v[9], &f->raMs) &&
                ToInt(v[11], &f->decMs) && ToDouble(v[16], &f->snr) && ToInt(v[17], &f->errorCode);
            f->raDir = v[10].empty() ? 0 : v[10][0];
            f->decDir = v[12].empty() ? 0 : v[12][0];
        }
        if (!ok)
            *error = "line " + std::to_string(lineNo) + ": bad frame row: " + line;
        return ok;
    }

} // namespace

Algorithm::Algorithm()
    : minMove(std::numeric_limits<double>::quiet_NaN()), hysteresis(std::numeric_limits<double>::quiet_NaN()),
      aggression(std::numeric_limits<double>::quiet_NaN()), slopeWeight(std::numeric_limits<double>::quiet_NaN()),
      aggressiveness(std::numeric_limits<double>::quiet_NaN())
{
}

int Session::FrameCount() const
{
    int n = 0;
    for (const Event& e : events)
        n += e.kind == Event::FrameRow && !e.frame.dropped;
    return n;
}

bool Parse(std::istream& in, std::vector<Session> *sessions, std::string *error)
{
    sessions->clear();
    std::string line;
    int lineNo = 0;
    Session *cur = nullptr;
    bool inHeader = false;
    while (std::getline(in, line))
    {
        lineNo++;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        if (StartsWith(line, "Guiding Begins at "))
        {
            sessions->emplace_back();
            cur = &sessions->back();
            cur->line = lineNo;
            cur->begins = line.substr(18);
            inHeader = true;
            continue;
        }
        if (StartsWith(line, "Guiding Ends at ") || StartsWith(line, "Calibration Begins at "))
        {
            cur = nullptr;
            inHeader = false;
            continue;
        }
        if (!cur)
            continue;

        if (inHeader)
        {
            if (StartsWith(line, "Frame,Time,"))
                inHeader = false;
            else if (StartsWith(line, "Pixel scale = "))
                Number(line, "Pixel scale", &cur->pixelScale);
            else if (StartsWith(line, "Exposure = "))
                Number(line, "Exposure", &cur->exposureMs);
            else if (StartsWith(line, "Mount = ") || StartsWith(line, "AO = "))
            {
                cur->mount = StartsWith(line, "Mount = ");
                cur->guidingEnabled = line.find("guiding enabled") != std::string::npos;
                Number(line, "xAngle", &cur->xAngle);
                Number(line, "xRate", &cur->xRate);
                Number(line, "yAngle", &cur->yAngle);
                Number(line, "yRate", &cur->yRate);
            }
            else if (StartsWith(line, "X guide algorithm = "))
                cur->x = ParseAlgorithm(line);
            else if (StartsWith(line, "Y guide algorithm = "))
                cur->y = ParseAlgorithm(line);
            else if (StartsWith(line, "Backlash comp = "))
            {
                cur->backlashComp = StartsWith(line, "Backlash comp = enabled");
                double pulse = 0.;
                Number(line, "pulse", &pulse);
                cur->backlashPulseMs = (int) pulse;
            }
            else if (StartsWith(line, "Max RA duration = "))
            {
                double v = 0.;
                if (Number(line, "Max RA duration", &v))
                    cur->maxRaMs = (int) v;
                if (Number(line, "Max DEC duration", &v))
                    cur->maxDecMs = (int) v;
                Word(line, "DEC guide mode", &cur->decGuideMode);
            }
            else if (StartsWith(line, "RA = "))
                Number(line, "Dec", &cur->dec);
            continue;
        }

        Event ev;
        ev.line = lineNo;
        if (!line.empty() && line[0] >= '0' && line[0] <= '9')
        {
            ev.kind = Event::FrameRow;
            if (!ParseFrame(line, lineNo, &ev.frame, error))
                return false;
        }
        else if (StartsWith(line, "INFO: DITHER by "))
        {
            // "INFO: DITHER by <ra>, <dec>, new lock pos = <x>, <y>": the dither in mount axes
            // (GuidingLog::NotifyGuidingDithered, from Mount::NotifyGuidingDithered's dx, dy)
            ev.kind = Event::Dither;
            char *end = nullptr;
            ev.ditherRa = std::strtod(line.c_str() + 16, &end);
            if (*end == ',')
                ev.ditherDec = std::strtod(end + 1, nullptr);
        }
        else if (StartsWith(line, "INFO: Guiding parameter change, MountGuidingEnabled = true"))
            ev.kind = Event::GuidingEnabled;
        else if (StartsWith(line, "INFO: Guiding parameter change, MountGuidingEnabled = false"))
            ev.kind = Event::GuidingDisabled;
        else if (StartsWith(line, "INFO: SETTLING STATE CHANGE, Settling started"))
            ev.kind = Event::SettleBegin;
        else if (StartsWith(line, "INFO: SETTLING STATE CHANGE, Settling"))
            ev.kind = Event::SettleEnd;
        else
            continue;
        cur->events.push_back(ev);
    }
    return true;
}

bool ParseFile(const std::string& path, std::vector<Session> *sessions, std::string *error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        *error = "cannot open " + path;
        return false;
    }
    return Parse(in, sessions, error);
}

} // namespace guide_log
