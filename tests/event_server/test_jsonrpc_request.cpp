// Event-server request checks (#118).
//
// Before a JSON-RPC request on :4400 is dispatched, handle_request checks
// that it has a string method and that params, when present, are an array
// or an object. A rejected request used to be answered with "id":null, so a
// client that matches replies by id (Ara, scripts/phd2_rpc_smoke.py) never
// saw the error and waited out its timeout. The error now carries the
// request's own id, and scalar or null params get -32602. The checks live
// in jsonrpc_request.cpp.

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "json_parser.h"
#include "jsonrpc_request.h"

namespace
{

const int kInvalidRequest = -32600;
const int kInvalidParams = -32602;

struct Checked
{
    JsonParser parser;
    JsonRpcRequestCheck chk;
};

// parses one request line and checks its root
void Check(Checked& c, const std::string& line)
{
    ASSERT_TRUE(c.parser.Parse(line)) << line << ": " << c.parser.ErrorDesc();
    c.chk = CheckJsonRpcRequest(c.parser.Root());
}

void ExpectIntId(const JsonRpcRequestCheck& chk, int id)
{
    ASSERT_NE(chk.id, nullptr);
    ASSERT_EQ(chk.id->type, JSON_INT);
    EXPECT_EQ(chk.id->int_value, id);
}

} // namespace

TEST(JsonRpcRequest, CodesAreTheJsonRpcOnes)
{
    EXPECT_EQ(JsonRpcRequest::kInvalidRequest, kInvalidRequest);
    EXPECT_EQ(JsonRpcRequest::kInvalidParams, kInvalidParams);
}

TEST(JsonRpcRequest, IssueReproScalarParamsGetInvalidParamsWithTheId)
{
    Checked c;
    Check(c, R"({"jsonrpc":"2.0","method":"set_exposure","params":2000,"id":42})");
    EXPECT_EQ(c.chk.error, kInvalidParams);
    ASSERT_NE(c.chk.message, nullptr);
    EXPECT_STREQ(c.chk.message, "invalid params - params must be an array or object");
    ExpectIntId(c.chk, 42);
    ASSERT_NE(c.chk.method, nullptr);
    EXPECT_STREQ(c.chk.method->string_value, "set_exposure");
}

TEST(JsonRpcRequest, EveryNonStructuredParamsGetsInvalidParamsWithTheId)
{
    const char *const params[] = { R"("2000")", "null", "true", "false", "1.5", "-3" };
    // the check does not look at the method; a few methods pin that it stays so
    const char *const methods[] = { "set_exposure", "guide", "set_connected", "get_app_state" };
    int id = 100;
    for (const char *m : methods)
    {
        for (const char *p : params)
        {
            std::string line = std::string(R"({"jsonrpc":"2.0","method":")") + m + R"(","params":)" + p + R"(,"id":)" +
                std::to_string(id) + "}";
            Checked c;
            Check(c, line);
            EXPECT_EQ(c.chk.error, kInvalidParams) << line;
            ExpectIntId(c.chk, id);
            ++id;
        }
    }
}

TEST(JsonRpcRequest, StringAndNullIdsAreEchoedAsGiven)
{
    {
        Checked c;
        Check(c, R"({"jsonrpc":"2.0","method":"set_exposure","params":"2000","id":"abc"})");
        EXPECT_EQ(c.chk.error, kInvalidParams);
        ASSERT_NE(c.chk.id, nullptr);
        ASSERT_EQ(c.chk.id->type, JSON_STRING);
        EXPECT_STREQ(c.chk.id->string_value, "abc");
    }
    {
        Checked c;
        Check(c, R"({"jsonrpc":"2.0","method":"set_exposure","params":2000,"id":null})");
        EXPECT_EQ(c.chk.error, kInvalidParams);
        ASSERT_NE(c.chk.id, nullptr);
        EXPECT_EQ(c.chk.id->type, JSON_NULL);
    }
}

TEST(JsonRpcRequest, ArrayObjectAndOmittedParamsPass)
{
    {
        Checked c;
        Check(c, R"({"jsonrpc":"2.0","method":"set_exposure","params":[2000],"id":1})");
        EXPECT_EQ(c.chk.error, 0);
        EXPECT_EQ(c.chk.message, nullptr);
        ASSERT_NE(c.chk.params, nullptr);
        EXPECT_EQ(c.chk.params->type, JSON_ARRAY);
        ExpectIntId(c.chk, 1);
    }
    {
        Checked c;
        Check(c, R"({"jsonrpc":"2.0","method":"set_exposure","params":[],"id":2})");
        EXPECT_EQ(c.chk.error, 0);
        ASSERT_NE(c.chk.params, nullptr);
        EXPECT_EQ(c.chk.params->type, JSON_ARRAY);
    }
    {
        Checked c;
        Check(c, R"({"jsonrpc":"2.0","method":"guide","params":{},"id":3})");
        EXPECT_EQ(c.chk.error, 0);
        ASSERT_NE(c.chk.params, nullptr);
        EXPECT_EQ(c.chk.params->type, JSON_OBJECT);
        ASSERT_NE(c.chk.method, nullptr);
        EXPECT_STREQ(c.chk.method->string_value, "guide");
    }
    {
        Checked c;
        Check(c, R"({"jsonrpc":"2.0","method":"get_app_state","id":4})");
        EXPECT_EQ(c.chk.error, 0);
        EXPECT_EQ(c.chk.params, nullptr);
        ExpectIntId(c.chk, 4);
    }
}

TEST(JsonRpcRequest, MissingOrNonStringMethodGetsInvalidRequestWithTheId)
{
    const char *const lines[] = {
        R"({"jsonrpc":"2.0","id":7})",
        R"({"jsonrpc":"2.0","method":5,"id":7})",
        R"({"jsonrpc":"2.0","method":null,"params":[],"id":7})",
        // a missing method is reported before bad params
        R"({"jsonrpc":"2.0","params":2000,"id":7})",
    };
    for (const char *line : lines)
    {
        Checked c;
        Check(c, line);
        EXPECT_EQ(c.chk.error, kInvalidRequest) << line;
        ASSERT_NE(c.chk.message, nullptr);
        EXPECT_STREQ(c.chk.message, "invalid request - missing method");
        EXPECT_EQ(c.chk.method, nullptr);
        ExpectIntId(c.chk, 7);
    }
}

TEST(JsonRpcRequest, NonObjectBatchElementsGetInvalidRequestWithNullId)
{
    JsonParser parser;
    std::string line = R"([2000,"x",null,[],true,{"jsonrpc":"2.0","method":"get_app_state","id":9}])";
    ASSERT_TRUE(parser.Parse(line));
    std::vector<JsonRpcRequestCheck> checks;
    json_for_each(req, parser.Root()) checks.push_back(CheckJsonRpcRequest(req));
    ASSERT_EQ(checks.size(), 6u);
    for (size_t i = 0; i < 5; i++)
    {
        EXPECT_EQ(checks[i].error, kInvalidRequest) << i;
        ASSERT_NE(checks[i].message, nullptr);
        EXPECT_STREQ(checks[i].message, "invalid request - request must be an object");
        EXPECT_EQ(checks[i].id, nullptr) << i;
        EXPECT_EQ(checks[i].method, nullptr) << i;
    }
    EXPECT_EQ(checks[5].error, 0);
    ExpectIntId(checks[5], 9);
}

TEST(JsonRpcRequest, NullRequestGetsInvalidRequest)
{
    JsonRpcRequestCheck chk = CheckJsonRpcRequest(nullptr);
    EXPECT_EQ(chk.error, kInvalidRequest);
    EXPECT_EQ(chk.id, nullptr);
}

TEST(JsonRpcRequest, BadParamsWithoutIdStillGetAnError)
{
    // handle_request answers this with "id":null: a refusal is never silent
    Checked c;
    Check(c, R"({"jsonrpc":"2.0","method":"set_exposure","params":2000})");
    EXPECT_EQ(c.chk.error, kInvalidParams);
    EXPECT_EQ(c.chk.id, nullptr);
}

TEST(JsonRpcRequest, LastDuplicateMemberWins)
{
    Checked c;
    Check(c, R"({"jsonrpc":"2.0","method":"a","method":"b","params":2000,"params":[1],"id":1,"id":2})");
    EXPECT_EQ(c.chk.error, 0);
    ASSERT_NE(c.chk.method, nullptr);
    EXPECT_STREQ(c.chk.method->string_value, "b");
    ASSERT_NE(c.chk.params, nullptr);
    EXPECT_EQ(c.chk.params->type, JSON_ARRAY);
    ExpectIntId(c.chk, 2);
}

TEST(JsonRpcRequest, ANonStringMethodDoesNotHideAStringOne)
{
    // as upstream parse_request: only a string "method" member is taken
    Checked c;
    Check(c, R"({"jsonrpc":"2.0","method":"get_app_state","method":5,"id":1})");
    EXPECT_EQ(c.chk.error, 0);
    ASSERT_NE(c.chk.method, nullptr);
    EXPECT_STREQ(c.chk.method->string_value, "get_app_state");
}

// The param readers the method handlers use (#138). set_algo_param read
// "value" with float_param without checking it was there, and float_param
// dereferenced the null param: {"axis":"ra","name":"minMove"} crashed the
// guider. A missing param is now false, like a param of the wrong type, and
// the handler answers -32602.
namespace
{

// the member of params named name, null when absent (as Params::param())
const json_value *Member(const json_value *params, const char *name)
{
    const json_value *found = nullptr;
    json_for_each(t, params)
    {
        if (t->name && std::strcmp(t->name, name) == 0)
            found = t;
    }
    return found;
}

const json_value *ParseValue(JsonParser& parser, const std::string& value)
{
    const std::string doc = R"({"value":)" + value + "}";
    if (!parser.Parse(doc))
    {
        ADD_FAILURE() << doc << ": " << parser.ErrorDesc();
        return nullptr;
    }
    return Member(parser.Root(), "value");
}

} // namespace

// The reader set_algo_param hands its value to; the handler's -32602 answer
// and that the guider keeps running are checked by scripts/phd2_rpc_smoke.py.
TEST(JsonRpcParams, ASetAlgoParamWithoutValueIsRefusedNotDereferenced)
{
    JsonParser parser;
    ASSERT_TRUE(parser.Parse(std::string(R"({"axis":"ra","name":"minMove"})")));
    const json_value *value = Member(parser.Root(), "value");
    ASSERT_EQ(value, nullptr);

    double v = 7.;
    EXPECT_FALSE(JsonRpcFloatParam(value, &v));
    EXPECT_EQ(v, 7.); // untouched
}

TEST(JsonRpcParams, AMissingParamIsFalseForEveryReader)
{
    double d = 1.;
    bool b = true;
    EXPECT_FALSE(JsonRpcFloatParam(nullptr, &d));
    EXPECT_FALSE(JsonRpcBoolParam(nullptr, &b));
    EXPECT_EQ(d, 1.);
    EXPECT_TRUE(b);
}

TEST(JsonRpcParams, FloatParamTakesIntsAndFloatsOnly)
{
    struct Case
    {
        const char *json;
        bool ok;
        double value;
    };
    const Case cases[] = {
        { "0", true, 0. },      { "3", true, 3. },         { "-12", true, -12. }, { "0.25", true, 0.25 },
        { "-1.5", true, -1.5 }, { "1e2", true, 100. },     { "true", false, 0. }, { "false", false, 0. },
        { "null", false, 0. },  { R"("1.5")", false, 0. }, { "[1]", false, 0. },  { R"({"v":1})", false, 0. },
    };
    for (const Case& c : cases)
    {
        JsonParser parser;
        const json_value *jv = ParseValue(parser, c.json);
        ASSERT_NE(jv, nullptr) << c.json;
        double v = -999.;
        EXPECT_EQ(JsonRpcFloatParam(jv, &v), c.ok) << c.json;
        if (c.ok)
            EXPECT_FLOAT_EQ(v, c.value) << c.json;
        else
            EXPECT_EQ(v, -999.) << c.json;
    }
}

TEST(JsonRpcParams, BoolParamTakesBoolsAndInts)
{
    struct Case
    {
        const char *json;
        bool ok;
        bool value;
    };
    const Case cases[] = {
        { "true", true, true }, { "false", true, false }, { "1", true, true },      { "0", true, false },
        { "-2", true, true },   { "1.0", false, false },  { "null", false, false }, { R"("true")", false, false },
        { "[]", false, false }, { "{}", false, false },
    };
    for (const Case& c : cases)
    {
        JsonParser parser;
        const json_value *jv = ParseValue(parser, c.json);
        ASSERT_NE(jv, nullptr) << c.json;
        bool v = !c.value;
        EXPECT_EQ(JsonRpcBoolParam(jv, &v), c.ok) << c.json;
        if (c.ok)
            EXPECT_EQ(v, c.value) << c.json;
        else
            EXPECT_EQ(v, !c.value) << c.json; // untouched
    }
}
