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
