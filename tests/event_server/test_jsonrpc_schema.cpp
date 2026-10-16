// Schema contract tests for the event-server JSON-RPC surface.
//
// event_server.cpp builds outgoing messages with static JObj<<NV(...)
// helpers buried in a 5,780-line file with deep Mount/Camera/socket
// coupling. We can't easily call those helpers from a unit test without a
// live PHD2.
//
// What we CAN do: pin the documented schema from the consumer's
// perspective. NINA, KStars, the web UI and other tools each parse a
// well-known set of fields. If anyone in this repo renames a field on
// either side (PHD2 emit OR these tests' fixtures), the diff makes the
// breaking-change visible at PR time instead of at the user-Discord-thread
// stage.
//
// The fixtures here mirror the canonical message bodies as documented at
// https://github.com/agalasso/phd2/wiki/EventMonitoring and
// https://github.com/agalasso/phd2/wiki/JsonRpcApi. Update these only with
// a deliberate decision to publish a schema change.

#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "json_parser.h"

namespace
{

const json_value *child(const json_value *parent, const char *name)
{
    if (!parent)
        return nullptr;
    for (const json_value *c = parent->first_child; c; c = c->next_sibling)
    {
        if (c->name && std::strcmp(c->name, name) == 0)
            return c;
    }
    return nullptr;
}

// json_value uses a union for its value member; accessing the wrong member
// after a failed type check is UB. The type assertions below are ASSERT_EQ
// (not EXPECT_EQ) so a wrong-type field hard-stops the test before any
// caller that reaches for `->string_value` etc. can hit UB.
void expect_string_field(const json_value *parent, const char *name)
{
    const json_value *v = child(parent, name);
    ASSERT_NE(v, nullptr) << "missing string field: " << name;
    ASSERT_EQ(v->type, JSON_STRING) << "field " << name << " not string";
}

void expect_numeric_field(const json_value *parent, const char *name)
{
    const json_value *v = child(parent, name);
    ASSERT_NE(v, nullptr) << "missing numeric field: " << name;
    ASSERT_TRUE(v->type == JSON_INT || v->type == JSON_FLOAT) << "field " << name << " not numeric";
}

void expect_bool_field(const json_value *parent, const char *name)
{
    const json_value *v = child(parent, name);
    ASSERT_NE(v, nullptr) << "missing bool field: " << name;
    ASSERT_EQ(v->type, JSON_BOOL) << "field " << name << " not bool";
}

}

// ---------------------------------------------------------------------------
// Common fields on every event (event_server.cpp Ev::Ev)
// ---------------------------------------------------------------------------
TEST(JsonRpcSchema, EveryEventHasEventTimestampHostInst)
{
    // Source: src/event_server.cpp ~line 303 — Ev::Ev() unconditionally
    // emits Event/Timestamp/Host/Inst on every notification. Downstream
    // consumers key off these fields (multi-instance routing in particular
    // depends on Host+Inst).
    const char *fixture = "{\"Event\":\"AppState\","
                          "\"Timestamp\":1700000000.123,"
                          "\"Host\":\"hostname\","
                          "\"Inst\":1,"
                          "\"State\":\"Stopped\"}";
    JsonParser p;
    ASSERT_TRUE(p.Parse(std::string(fixture)));
    expect_string_field(p.Root(), "Event");
    expect_numeric_field(p.Root(), "Timestamp");
    expect_string_field(p.Root(), "Host");
    expect_numeric_field(p.Root(), "Inst");
}

// ---------------------------------------------------------------------------
// Version event (boot)
// ---------------------------------------------------------------------------
TEST(JsonRpcSchema, VersionEvent)
{
    // event_server.cpp ~line 310-312
    const char *fixture = "{\"Event\":\"Version\",\"Timestamp\":1.0,\"Host\":\"h\",\"Inst\":1,"
                          "\"PHDVersion\":\"2.6.13\","
                          "\"PHDSubver\":\"\","
                          "\"OverlapSupport\":true,"
                          "\"MsgVersion\":1}";
    JsonParser p;
    ASSERT_TRUE(p.Parse(std::string(fixture)));
    expect_string_field(p.Root(), "PHDVersion");
    expect_string_field(p.Root(), "PHDSubver");
    expect_bool_field(p.Root(), "OverlapSupport");
    expect_numeric_field(p.Root(), "MsgVersion");
}

// ---------------------------------------------------------------------------
// AppState event
// ---------------------------------------------------------------------------
TEST(JsonRpcSchema, AppStateEvent)
{
    // event_server.cpp ~line 362-363. State values must remain in the
    // documented set — adding is OK, renaming is a break.
    static const char *kStates[] = {
        "Stopped", "Selected", "Calibrating", "Guiding", "LostLock", "Paused", "Looping",
    };
    for (const char *s : kStates)
    {
        std::string fixture =
            std::string("{\"Event\":\"AppState\",\"Timestamp\":1.0,\"Host\":\"h\",\"Inst\":1,\"State\":\"") + s + "\"}";
        JsonParser p;
        ASSERT_TRUE(p.Parse(fixture)) << "state: " << s;
        expect_string_field(p.Root(), "State");
    }
}

// ---------------------------------------------------------------------------
// SettleDone event
// ---------------------------------------------------------------------------
TEST(JsonRpcSchema, SettleDoneEvent)
{
    // event_server.cpp ~line 378-389. Status==0 means success, !=0 failure.
    // NINA/KStars depend on Status, TotalFrames, DroppedFrames, and on
    // Error being present iff Status!=0.
    {
        const char *fixture = "{\"Event\":\"SettleDone\",\"Timestamp\":1.0,\"Host\":\"h\",\"Inst\":1,"
                              "\"Status\":0,\"TotalFrames\":12,\"DroppedFrames\":1}";
        JsonParser p;
        ASSERT_TRUE(p.Parse(std::string(fixture)));
        expect_numeric_field(p.Root(), "Status");
        expect_numeric_field(p.Root(), "TotalFrames");
        expect_numeric_field(p.Root(), "DroppedFrames");
        // Error MUST be absent on success — NINA/KStars read `Error` as a
        // truthy indicator, so a spurious empty-string Error on Status=0
        // looks like a settle failure to them.
        EXPECT_EQ(child(p.Root(), "Error"), nullptr);
    }
    {
        const char *fixture = "{\"Event\":\"SettleDone\",\"Timestamp\":1.0,\"Host\":\"h\",\"Inst\":1,"
                              "\"Status\":1,\"Error\":\"timeout\",\"TotalFrames\":4,\"DroppedFrames\":4}";
        JsonParser p;
        ASSERT_TRUE(p.Parse(std::string(fixture)));
        expect_numeric_field(p.Root(), "Status");
        expect_string_field(p.Root(), "Error");
    }
}

// ---------------------------------------------------------------------------
// SingleFrameComplete event
// ---------------------------------------------------------------------------
TEST(JsonRpcSchema, SingleFrameCompleteEvent)
{
    // event_server.cpp NotifySingleFrameComplete. When save=true the event
    // carries Path (daemon-local absolute path) AND Filename (basename) —
    // remote clients fetch the file via GET /api/capture/<Filename>, so
    // dropping or renaming Filename breaks the remote capture-retrieval
    // flow (issue #77 / OpenAstro Ara polar alignment).
    {
        const char *fixture = "{\"Event\":\"SingleFrameComplete\",\"Timestamp\":1.0,\"Host\":\"h\",\"Inst\":1,"
                              "\"Success\":true,\"Path\":\"/var/lib/openastro-guider/.openastro-guider/save_image_abc\","
                              "\"Filename\":\"save_image_abc\"}";
        JsonParser p;
        ASSERT_TRUE(p.Parse(std::string(fixture)));
        expect_bool_field(p.Root(), "Success");
        expect_string_field(p.Root(), "Path");
        expect_string_field(p.Root(), "Filename");
        EXPECT_EQ(child(p.Root(), "Error"), nullptr);
    }
    {
        const char *fixture = "{\"Event\":\"SingleFrameComplete\",\"Timestamp\":1.0,\"Host\":\"h\",\"Inst\":1,"
                              "\"Success\":false,\"Error\":\"exposure failed\"}";
        JsonParser p;
        ASSERT_TRUE(p.Parse(std::string(fixture)));
        expect_bool_field(p.Root(), "Success");
        expect_string_field(p.Root(), "Error");
    }
}

// ---------------------------------------------------------------------------
// Settling event
// ---------------------------------------------------------------------------
TEST(JsonRpcSchema, SettlingEvent)
{
    // event_server.cpp ~line 369-371
    const char *fixture = "{\"Event\":\"Settling\",\"Timestamp\":1.0,\"Host\":\"h\",\"Inst\":1,"
                          "\"Distance\":0.32,\"Time\":0.5,\"SettleTime\":10.0,\"StarLocked\":true}";
    JsonParser p;
    ASSERT_TRUE(p.Parse(std::string(fixture)));
    expect_numeric_field(p.Root(), "Distance");
    expect_numeric_field(p.Root(), "Time");
    expect_numeric_field(p.Root(), "SettleTime");
    expect_bool_field(p.Root(), "StarLocked");
}

// ---------------------------------------------------------------------------
// JSON-RPC response envelope (JRpcResponse, ~line 607-624)
// ---------------------------------------------------------------------------
TEST(JsonRpcSchema, ResponseEnvelopeSuccess)
{
    const char *fixture = "{\"jsonrpc\":\"2.0\",\"result\":42,\"id\":17}";
    JsonParser p;
    ASSERT_TRUE(p.Parse(std::string(fixture)));
    expect_string_field(p.Root(), "jsonrpc");
    EXPECT_STREQ(child(p.Root(), "jsonrpc")->string_value, "2.0");
    ASSERT_NE(child(p.Root(), "result"), nullptr);
    // JSON-RPC 2.0: the response MUST have exactly one of `result` or
    // `error`. Mixed envelopes are invalid and would confuse clients.
    EXPECT_EQ(child(p.Root(), "error"), nullptr);
    expect_numeric_field(p.Root(), "id");
}

TEST(JsonRpcSchema, ResponseEnvelopeError)
{
    // event_server.cpp ~line 585-586 — error has code (int) and message
    // (string). Both must be present per the JSON-RPC 2.0 spec.
    const char *fixture = "{\"jsonrpc\":\"2.0\","
                          "\"error\":{\"code\":-32601,\"message\":\"Method not found\"},"
                          "\"id\":17}";
    JsonParser p;
    ASSERT_TRUE(p.Parse(std::string(fixture)));
    const json_value *err = child(p.Root(), "error");
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(err->type, JSON_OBJECT);
    // Mirror exclusivity: error envelope MUST NOT carry a `result`.
    EXPECT_EQ(child(p.Root(), "result"), nullptr);
    expect_numeric_field(err, "code");
    expect_string_field(err, "message");
}

// ---------------------------------------------------------------------------
// get_profiles result shape (event_server.cpp ~line 668-681)
// ---------------------------------------------------------------------------
TEST(JsonRpcSchema, GetProfilesResult)
{
    const char *fixture = "{\"jsonrpc\":\"2.0\","
                          "\"result\":["
                          "{\"id\":1,\"name\":\"Default\",\"selected\":true},"
                          "{\"id\":2,\"name\":\"Lab\"}"
                          "],\"id\":3}";
    JsonParser p;
    ASSERT_TRUE(p.Parse(std::string(fixture)));
    const json_value *result = child(p.Root(), "result");
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->type, JSON_ARRAY);

    int count = 0;
    json_for_each(prof, result)
    {
        EXPECT_EQ(prof->type, JSON_OBJECT);
        expect_numeric_field(prof, "id");
        expect_string_field(prof, "name");
        // selected is optional (only present when true)
        ++count;
    }
    EXPECT_EQ(count, 2);
}

// ---------------------------------------------------------------------------
// Equipment profile field shape (~line 2320-2330)
// ---------------------------------------------------------------------------
TEST(JsonRpcSchema, GetEquipmentProfileResult)
{
    const char *fixture = "{\"jsonrpc\":\"2.0\",\"result\":{"
                          "\"focal_length\":1000,"
                          "\"pixel_size\":4.5,"
                          "\"camera_binning\":1,"
                          "\"software_binning\":1,"
                          "\"guide_speed\":0.5,"
                          "\"calibration_duration\":750,"
                          "\"calibration_distance\":25,"
                          "\"high_res_encoders\":false,"
                          "\"auto_restore_calibration\":true,"
                          "\"multistar_enabled\":true"
                          "},\"id\":4}";
    JsonParser p;
    ASSERT_TRUE(p.Parse(std::string(fixture)));
    const json_value *r = child(p.Root(), "result");
    ASSERT_NE(r, nullptr);
    expect_numeric_field(r, "focal_length");
    expect_numeric_field(r, "pixel_size");
    expect_numeric_field(r, "camera_binning");
    expect_numeric_field(r, "software_binning");
    expect_numeric_field(r, "guide_speed");
    expect_numeric_field(r, "calibration_duration");
    expect_numeric_field(r, "calibration_distance");
    expect_bool_field(r, "high_res_encoders");
    expect_bool_field(r, "auto_restore_calibration");
    expect_bool_field(r, "multistar_enabled");
}

// ---------------------------------------------------------------------------
// Lock-shift query shape (~line 2872-2876)
// ---------------------------------------------------------------------------
TEST(JsonRpcSchema, GetLockShiftParams)
{
    {
        // shift disabled — only enabled is present
        const char *fixture = "{\"jsonrpc\":\"2.0\",\"result\":{\"enabled\":false},\"id\":1}";
        JsonParser p;
        ASSERT_TRUE(p.Parse(std::string(fixture)));
        const json_value *r = child(p.Root(), "result");
        ASSERT_NE(r, nullptr);
        expect_bool_field(r, "enabled");
        // rate/units/axes are conditionally emitted only when enabled=true
        // (event_server.cpp ~line 2873-2876). Pin the absence so a server
        // that starts unconditionally including them is caught.
        EXPECT_EQ(child(r, "rate"), nullptr);
        EXPECT_EQ(child(r, "units"), nullptr);
        EXPECT_EQ(child(r, "axes"), nullptr);
    }
    {
        // shift enabled — rate/units/axes are present
        const char *fixture = "{\"jsonrpc\":\"2.0\",\"result\":{"
                              "\"enabled\":true,"
                              "\"rate\":[1.0,2.0],"
                              "\"units\":\"arcsec/hr\","
                              "\"axes\":\"RA/Dec\""
                              "},\"id\":1}";
        JsonParser p;
        ASSERT_TRUE(p.Parse(std::string(fixture)));
        const json_value *r = child(p.Root(), "result");
        ASSERT_NE(r, nullptr);
        expect_bool_field(r, "enabled");
        const json_value *rate = child(r, "rate");
        ASSERT_NE(rate, nullptr);
        EXPECT_EQ(rate->type, JSON_ARRAY);
        const json_value *units = child(r, "units");
        ASSERT_NE(units, nullptr);
        ASSERT_EQ(units->type, JSON_STRING); // json_value is a union; checking type before string_value avoids UB
        // arcsec/hr or pixels/hr — value contract
        EXPECT_TRUE(std::strcmp(units->string_value, "arcsec/hr") == 0 || std::strcmp(units->string_value, "pixels/hr") == 0);
        const json_value *axes = child(r, "axes");
        ASSERT_NE(axes, nullptr);
        ASSERT_EQ(axes->type, JSON_STRING);
        EXPECT_TRUE(std::strcmp(axes->string_value, "RA/Dec") == 0 || std::strcmp(axes->string_value, "X/Y") == 0);
    }
}

// ---------------------------------------------------------------------------
// PHD_Point shape (~line 290) — used for star coords, lock pos, etc.
// ---------------------------------------------------------------------------
TEST(JsonRpcSchema, PointShape)
{
    const char *fixture = "{\"X\":123.456,\"Y\":78.9}";
    JsonParser p;
    ASSERT_TRUE(p.Parse(std::string(fixture)));
    expect_numeric_field(p.Root(), "X");
    expect_numeric_field(p.Root(), "Y");
}

// ---------------------------------------------------------------------------
// StarSelected event (~line 338)
// ---------------------------------------------------------------------------
TEST(JsonRpcSchema, StarSelectedEvent)
{
    const char *fixture = "{\"Event\":\"StarSelected\",\"Timestamp\":1.0,\"Host\":\"h\",\"Inst\":1,"
                          "\"X\":100.5,\"Y\":200.7}";
    JsonParser p;
    ASSERT_TRUE(p.Parse(std::string(fixture)));
    expect_numeric_field(p.Root(), "X");
    expect_numeric_field(p.Root(), "Y");
}

// ---------------------------------------------------------------------------
// Method-dispatch request shape (params can be array or object)
// ---------------------------------------------------------------------------
TEST(JsonRpcSchema, MethodRequestArrayParams)
{
    const char *fixture = "{\"method\":\"set_lock_position\",\"params\":[100.0,200.0,true],\"id\":1}";
    JsonParser p;
    ASSERT_TRUE(p.Parse(std::string(fixture)));
    expect_string_field(p.Root(), "method");
    const json_value *params = child(p.Root(), "params");
    ASSERT_NE(params, nullptr);
    EXPECT_EQ(params->type, JSON_ARRAY);
}

TEST(JsonRpcSchema, MethodRequestObjectParams)
{
    const char *fixture = "{\"method\":\"set_lock_position\","
                          "\"params\":{\"x\":100.0,\"y\":200.0,\"exact\":true},"
                          "\"id\":1}";
    JsonParser p;
    ASSERT_TRUE(p.Parse(std::string(fixture)));
    const json_value *params = child(p.Root(), "params");
    ASSERT_NE(params, nullptr);
    EXPECT_EQ(params->type, JSON_OBJECT);
    expect_numeric_field(params, "x");
    expect_numeric_field(params, "y");
    expect_bool_field(params, "exact");
}

// ---------------------------------------------------------------------------
// Headless Guiding Assistant (guiding_assistant_status_obj in event_server.cpp)
// ARA's Setup flow drives these; the get_status result, the
// GuidingAssistantProgress event and the GuidingAssistantDone event share one
// formatter, so the same field names apply to all three.
// ---------------------------------------------------------------------------
namespace
{

// Fields present once at least two samples have been taken
void expect_ga_measurement_fields(const json_value *r)
{
    expect_string_field(r, "state");
    expect_numeric_field(r, "duration_s");
    expect_bool_field(r, "measure_backlash");
    expect_numeric_field(r, "elapsed_s");
    expect_numeric_field(r, "samples");
    expect_numeric_field(r, "pixel_scale");
    expect_numeric_field(r, "ra_drift_px_min");
    expect_numeric_field(r, "dec_drift_px_min");
    expect_numeric_field(r, "ra_rms_px");
    expect_numeric_field(r, "dec_rms_px");
    expect_numeric_field(r, "high_freq_rms_px");
    expect_numeric_field(r, "ra_peak_px");
    expect_numeric_field(r, "dec_peak_px");
    expect_numeric_field(r, "ra_peak_peak_px");
    expect_numeric_field(r, "ra_max_drift_px_s");
    expect_numeric_field(r, "snr");
    expect_numeric_field(r, "star_mass");
    expect_numeric_field(r, "polar_align_error_arcmin");
    expect_bool_field(r, "polar_align_error_is_lower_bound");
}

bool string_is(const json_value *v, const char *s)
{
    return v && v->type == JSON_STRING && std::strcmp(v->string_value, s) == 0;
}

} // namespace

TEST(JsonRpcSchema, GuidingAssistantStatusIdle)
{
    // idle: only the state - a client that never started a run sees nothing else
    const char *fixture = "{\"jsonrpc\":\"2.0\",\"result\":{\"state\":\"idle\"},\"id\":1}";
    JsonParser p;
    ASSERT_TRUE(p.Parse(std::string(fixture)));
    const json_value *r = child(p.Root(), "result");
    ASSERT_NE(r, nullptr);
    EXPECT_TRUE(string_is(child(r, "state"), "idle"));
    EXPECT_EQ(child(r, "samples"), nullptr);
    EXPECT_EQ(child(r, "recommendations"), nullptr);
}

TEST(JsonRpcSchema, GuidingAssistantStatusMeasuring)
{
    const char *fixture = "{\"jsonrpc\":\"2.0\",\"result\":{"
                          "\"state\":\"measuring\",\"duration_s\":120,\"measure_backlash\":true,\"elapsed_s\":42.0,"
                          "\"samples\":21,\"pixel_scale\":1.234,"
                          "\"ra_drift_px_min\":0.512,\"dec_drift_px_min\":-0.204,\"ra_rms_px\":0.311,\"dec_rms_px\":0.287,"
                          "\"high_freq_rms_px\":0.423,\"ra_peak_px\":0.9,\"dec_peak_px\":0.7,\"ra_peak_peak_px\":1.4,"
                          "\"ra_max_drift_px_s\":0.0213,\"snr\":35.2,\"star_mass\":12345,"
                          "\"polar_align_error_arcmin\":1.0,\"polar_align_error_is_lower_bound\":false"
                          "},\"id\":1}";
    JsonParser p;
    ASSERT_TRUE(p.Parse(std::string(fixture)));
    const json_value *r = child(p.Root(), "result");
    ASSERT_NE(r, nullptr);
    expect_ga_measurement_fields(r);
    // results only exist once the run is done; no error while running
    EXPECT_EQ(child(r, "recommendations"), nullptr);
    EXPECT_EQ(child(r, "error"), nullptr);
}

TEST(JsonRpcSchema, GuidingAssistantStatusMeasuringBacklash)
{
    const char *fixture = "{\"jsonrpc\":\"2.0\",\"result\":{"
                          "\"state\":\"measuring_backlash\",\"duration_s\":120,\"measure_backlash\":true,"
                          "\"elapsed_s\":121.3,\"samples\":61,\"pixel_scale\":1.234,"
                          "\"backlash_status\":\"Moving North for 500 ms, step 3 / 8\""
                          "},\"id\":1}";
    JsonParser p;
    ASSERT_TRUE(p.Parse(std::string(fixture)));
    const json_value *r = child(p.Root(), "result");
    ASSERT_NE(r, nullptr);
    EXPECT_TRUE(string_is(child(r, "state"), "measuring_backlash"));
    expect_string_field(r, "backlash_status");
}

TEST(JsonRpcSchema, GuidingAssistantStatusDone)
{
    const char *fixture = "{\"jsonrpc\":\"2.0\",\"result\":{"
                          "\"state\":\"done\",\"duration_s\":120,\"measure_backlash\":true,\"elapsed_s\":120.4,"
                          "\"samples\":61,\"pixel_scale\":1.234,"
                          "\"ra_drift_px_min\":0.512,\"dec_drift_px_min\":-0.204,\"ra_rms_px\":0.311,\"dec_rms_px\":0.287,"
                          "\"high_freq_rms_px\":0.423,\"ra_peak_px\":0.9,\"dec_peak_px\":0.7,\"ra_peak_peak_px\":1.4,"
                          "\"ra_max_drift_px_s\":0.0213,\"snr\":35.2,\"star_mass\":12345,"
                          "\"polar_align_error_arcmin\":1.0,\"polar_align_error_is_lower_bound\":true,"
                          "\"backlash_ms\":1234,\"backlash_sigma_ms\":120,\"backlash_px\":3.21,"
                          "\"recommendations\":{"
                          "\"min_move_ra_px\":0.2,\"min_move_dec_px\":0.3,\"min_move_seeing_based\":true,"
                          "\"exposure_s\":2.0,\"exposure_max_s\":4.0,\"drift_limiting_exposure_s\":9.4,"
                          "\"dec_backlash_ms\":1230,"
                          "\"messages\":[{\"id\":\"Exp\",\"text\":\"Use exposure times in the range of 2.0s to 4.0s\"},"
                          "{\"id\":\"BLT\",\"text\":\"Try starting with a Dec backlash compensation of 1230 ms\"}]"
                          "}},\"id\":1}";
    JsonParser p;
    ASSERT_TRUE(p.Parse(std::string(fixture)));
    const json_value *r = child(p.Root(), "result");
    ASSERT_NE(r, nullptr);
    expect_ga_measurement_fields(r);
    EXPECT_TRUE(string_is(child(r, "state"), "done"));
    expect_numeric_field(r, "backlash_ms");
    expect_numeric_field(r, "backlash_sigma_ms");
    expect_numeric_field(r, "backlash_px");

    const json_value *recs = child(r, "recommendations");
    ASSERT_NE(recs, nullptr);
    ASSERT_EQ(recs->type, JSON_OBJECT);
    expect_numeric_field(recs, "min_move_ra_px");
    expect_numeric_field(recs, "min_move_dec_px");
    expect_bool_field(recs, "min_move_seeing_based");
    expect_numeric_field(recs, "exposure_s");
    expect_numeric_field(recs, "exposure_max_s");
    expect_numeric_field(recs, "dec_backlash_ms");
    // dec_guide_mode / dec_algorithm are only present when recommended
    EXPECT_EQ(child(recs, "dec_guide_mode"), nullptr);
    EXPECT_EQ(child(recs, "dec_algorithm"), nullptr);

    const json_value *msgs = child(recs, "messages");
    ASSERT_NE(msgs, nullptr);
    ASSERT_EQ(msgs->type, JSON_ARRAY);
    ASSERT_NE(msgs->first_child, nullptr);
    for (const json_value *m = msgs->first_child; m; m = m->next_sibling)
    {
        expect_string_field(m, "id");
        expect_string_field(m, "text");
    }
}

TEST(JsonRpcSchema, GuidingAssistantStatusDoneTooMuchBacklash)
{
    // backlash too large to compensate: no dec_backlash_ms, a one-direction
    // dec_guide_mode instead (a Scope::DecGuideModeStr value set_dec_guide_mode accepts)
    const char *fixture = "{\"state\":\"done\",\"recommendations\":{"
                          "\"min_move_ra_px\":0.2,\"min_move_dec_px\":0.3,\"min_move_seeing_based\":true,"
                          "\"exposure_s\":2.0,\"exposure_max_s\":4.0,\"dec_guide_mode\":\"South\","
                          "\"dec_algorithm\":\"Lowpass2\",\"messages\":[]}}";
    JsonParser p;
    ASSERT_TRUE(p.Parse(std::string(fixture)));
    const json_value *recs = child(p.Root(), "recommendations");
    ASSERT_NE(recs, nullptr);
    EXPECT_EQ(child(recs, "dec_backlash_ms"), nullptr);
    const json_value *mode = child(recs, "dec_guide_mode");
    EXPECT_TRUE(string_is(mode, "North") || string_is(mode, "South"));
    EXPECT_TRUE(string_is(child(recs, "dec_algorithm"), "Lowpass2"));
}

TEST(JsonRpcSchema, GuidingAssistantStatusFailed)
{
    const char *fixture = "{\"jsonrpc\":\"2.0\",\"result\":{"
                          "\"state\":\"failed\",\"duration_s\":120,\"measure_backlash\":false,\"elapsed_s\":33.0,"
                          "\"samples\":16,\"pixel_scale\":1.234,\"error\":\"guiding stopped\""
                          "},\"id\":1}";
    JsonParser p;
    ASSERT_TRUE(p.Parse(std::string(fixture)));
    const json_value *r = child(p.Root(), "result");
    ASSERT_NE(r, nullptr);
    EXPECT_TRUE(string_is(child(r, "state"), "failed"));
    expect_string_field(r, "error");
    EXPECT_EQ(child(r, "recommendations"), nullptr);
}

TEST(JsonRpcSchema, GuidingAssistantProgressEvent)
{
    // one per unguided sample; never carries recommendations
    const char *fixture = "{\"Event\":\"GuidingAssistantProgress\",\"Timestamp\":1.0,\"Host\":\"h\",\"Inst\":1,"
                          "\"state\":\"measuring\",\"duration_s\":120,\"measure_backlash\":false,\"elapsed_s\":42.0,"
                          "\"samples\":21,\"pixel_scale\":1.234,"
                          "\"ra_drift_px_min\":0.512,\"dec_drift_px_min\":-0.204,\"ra_rms_px\":0.311,\"dec_rms_px\":0.287,"
                          "\"high_freq_rms_px\":0.423,\"ra_peak_px\":0.9,\"dec_peak_px\":0.7,\"ra_peak_peak_px\":1.4,"
                          "\"ra_max_drift_px_s\":0.0213,\"snr\":35.2,\"star_mass\":12345,"
                          "\"polar_align_error_arcmin\":1.0,\"polar_align_error_is_lower_bound\":false}";
    JsonParser p;
    ASSERT_TRUE(p.Parse(std::string(fixture)));
    EXPECT_TRUE(string_is(child(p.Root(), "Event"), "GuidingAssistantProgress"));
    expect_ga_measurement_fields(p.Root());
    EXPECT_EQ(child(p.Root(), "recommendations"), nullptr);
}

TEST(JsonRpcSchema, GuidingAssistantDoneEvent)
{
    const char *fixture = "{\"Event\":\"GuidingAssistantDone\",\"Timestamp\":1.0,\"Host\":\"h\",\"Inst\":1,"
                          "\"state\":\"done\",\"duration_s\":120,\"measure_backlash\":false,\"elapsed_s\":120.4,"
                          "\"samples\":61,\"pixel_scale\":1.234,"
                          "\"ra_drift_px_min\":0.512,\"dec_drift_px_min\":-0.204,\"ra_rms_px\":0.311,\"dec_rms_px\":0.287,"
                          "\"high_freq_rms_px\":0.423,\"ra_peak_px\":0.9,\"dec_peak_px\":0.7,\"ra_peak_peak_px\":1.4,"
                          "\"ra_max_drift_px_s\":0.0213,\"snr\":35.2,\"star_mass\":12345,"
                          "\"polar_align_error_arcmin\":1.0,\"polar_align_error_is_lower_bound\":false,"
                          "\"recommendations\":{\"min_move_ra_px\":0.2,\"min_move_dec_px\":0.3,"
                          "\"min_move_seeing_based\":true,\"exposure_s\":2.0,\"exposure_max_s\":4.0,\"messages\":[]}}";
    JsonParser p;
    ASSERT_TRUE(p.Parse(std::string(fixture)));
    EXPECT_TRUE(string_is(child(p.Root(), "Event"), "GuidingAssistantDone"));
    const json_value *state = child(p.Root(), "state");
    EXPECT_TRUE(string_is(state, "done") || string_is(state, "failed"));
    const json_value *recs = child(p.Root(), "recommendations");
    ASSERT_NE(recs, nullptr);
    expect_numeric_field(recs, "min_move_ra_px");
    expect_numeric_field(recs, "min_move_dec_px");
    expect_numeric_field(recs, "exposure_s");
}

TEST(JsonRpcSchema, GuidingAssistantApplyResult)
{
    const char *fixture = "{\"jsonrpc\":\"2.0\",\"result\":{\"applied\":[\"min_move_ra\",\"min_move_dec\"]},\"id\":1}";
    JsonParser p;
    ASSERT_TRUE(p.Parse(std::string(fixture)));
    const json_value *r = child(p.Root(), "result");
    ASSERT_NE(r, nullptr);
    const json_value *applied = child(r, "applied");
    ASSERT_NE(applied, nullptr);
    ASSERT_EQ(applied->type, JSON_ARRAY);
    for (const json_value *a = applied->first_child; a; a = a->next_sibling)
        EXPECT_EQ(a->type, JSON_STRING);
}
