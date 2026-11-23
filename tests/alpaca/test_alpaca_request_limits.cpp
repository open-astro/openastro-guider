// alpaca_request_limits (#294).
//
// One AlpacaClient request's own timeout and abort check, on a real curl handle. The server is a
// local socket that accepts connections (the kernel's backlog does it) and never answers, as the
// e2e fake does with a stalled PulseGuide: an abort check that turns true gives the request up
// within about a second, a request no check gives up waits for its own timeout, and the handle
// gets the client's timeout back and no abort check for the next request.

#include <gtest/gtest.h>

#include "alpaca_request_limits.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

namespace
{
std::atomic<bool> g_stop { false };

bool StopRequested()
{
    return g_stop.load();
}

bool NeverStop()
{
    return false;
}

bool AlwaysStop()
{
    return true;
}

// A listening socket on 127.0.0.1 that never accepts or answers: a connect succeeds and the
// request then waits for a response that never comes
class HeldServer
{
public:
    HeldServer()
    {
        m_fd = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (m_fd < 0 || bind(m_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 || listen(m_fd, 16) != 0)
            return;
        socklen_t len = sizeof(addr);
        if (getsockname(m_fd, reinterpret_cast<sockaddr *>(&addr), &len) == 0)
            m_port = ntohs(addr.sin_port);
    }
    ~HeldServer()
    {
        if (m_fd >= 0)
            close(m_fd);
    }
    HeldServer(const HeldServer&) = delete;
    HeldServer& operator=(const HeldServer&) = delete;
    int Port() const { return m_port; }

private:
    int m_fd = -1;
    int m_port = 0;
};

// A curl handle set up as AlpacaClient's PutAction sets it, with the client's timeout
class PutHandle
{
public:
    PutHandle(int port, long clientTimeoutMs)
        : m_url("http://127.0.0.1:" + std::to_string(port) + "/api/v1/telescope/0/pulseguide")
    {
        m_curl = curl_easy_init();
        curl_easy_setopt(m_curl, CURLOPT_URL, m_url.c_str());
        curl_easy_setopt(m_curl, CURLOPT_CUSTOMREQUEST, "PUT");
        curl_easy_setopt(m_curl, CURLOPT_POSTFIELDS, kBody);
        curl_easy_setopt(m_curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(sizeof(kBody) - 1));
        curl_easy_setopt(m_curl, CURLOPT_TIMEOUT_MS, clientTimeoutMs);
        curl_easy_setopt(m_curl, CURLOPT_CONNECTTIMEOUT_MS, 2000L);
        curl_easy_setopt(m_curl, CURLOPT_FRESH_CONNECT, 1L);
        curl_easy_setopt(m_curl, CURLOPT_FORBID_REUSE, 1L);
    }
    ~PutHandle() { curl_easy_cleanup(m_curl); }
    PutHandle(const PutHandle&) = delete;
    PutHandle& operator=(const PutHandle&) = delete;
    CURL *Get() const { return m_curl; }

private:
    std::string m_url;
    static constexpr char kBody[] = "ClientID=1&ClientTransactionID=1&Direction=3&Duration=200";
    CURL *m_curl = nullptr;
};

struct RequestRun
{
    CURLcode res = CURLE_OK;
    long ms = 0;
    bool aborted = false;
};

// One request on handle, with limits (or none) and the client's timeout
RequestRun Perform(CURL *handle, const AlpacaRequestLimits *limits, long clientTimeoutMs)
{
    RequestRun run;
    auto t0 = std::chrono::steady_clock::now();
    {
        AlpacaRequestLimitsScope scope(handle, limits, clientTimeoutMs);
        run.res = curl_easy_perform(handle);
        run.aborted = scope.Aborted();
    }
    run.ms =
        static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
    return run;
}

class AlpacaRequestLimitsTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite() { curl_global_init(CURL_GLOBAL_DEFAULT); }
    static void TearDownTestSuite() { curl_global_cleanup(); }
    void SetUp() override
    {
        g_stop = false;
        ASSERT_GT(m_server.Port(), 0);
    }
    HeldServer m_server;
};
} // namespace

TEST_F(AlpacaRequestLimitsTest, StopGivesUpAHeldRequestWithinAboutASecond)
{
    // stop_capture while the mount holds the PulseGuide: the worker's interrupt turns the check
    // true from the main thread, and the request is given up long before its 20 s timeout
    PutHandle put(m_server.Port(), 30000);
    AlpacaRequestLimits limits;
    limits.timeoutMs = 20000;
    limits.abortRequested = &StopRequested;
    std::thread stopper(
        []
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            g_stop = true;
        });
    RequestRun run = Perform(put.Get(), &limits, 30000);
    stopper.join();
    EXPECT_EQ(run.res, CURLE_ABORTED_BY_CALLBACK);
    EXPECT_TRUE(run.aborted);
    EXPECT_GE(run.ms, 250);
    EXPECT_LT(run.ms, 2500) << "the request was given up " << run.ms << " ms after it was sent";
}

TEST_F(AlpacaRequestLimitsTest, HeldRequestTimesOutAtItsOwnTimeout)
{
    // no stop: the request's own timeout, shorter than the client's, ends it
    PutHandle put(m_server.Port(), 30000);
    AlpacaRequestLimits limits;
    limits.timeoutMs = 1200;
    limits.abortRequested = &NeverStop;
    RequestRun run = Perform(put.Get(), &limits, 30000);
    EXPECT_EQ(run.res, CURLE_OPERATION_TIMEDOUT);
    EXPECT_FALSE(run.aborted);
    EXPECT_GE(run.ms, 1100);
    EXPECT_LT(run.ms, 5000);
}

TEST_F(AlpacaRequestLimitsTest, NextRequestHasTheClientsTimeoutAndNoAbortCheck)
{
    // A request given up at once, then one with no limits on the same handle: it is not given up
    // (the check is off the handle) and times out at the client's 1 s, not the first one's 20 s
    PutHandle put(m_server.Port(), 1000);
    AlpacaRequestLimits limits;
    limits.timeoutMs = 20000;
    limits.abortRequested = &AlwaysStop;
    RequestRun first = Perform(put.Get(), &limits, 1000);
    EXPECT_EQ(first.res, CURLE_ABORTED_BY_CALLBACK);
    EXPECT_TRUE(first.aborted);
    EXPECT_LT(first.ms, 2500);

    auto t0 = std::chrono::steady_clock::now();
    CURLcode res = curl_easy_perform(put.Get());
    long ms =
        static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
    EXPECT_EQ(res, CURLE_OPERATION_TIMEDOUT);
    EXPECT_GE(ms, 900);
    EXPECT_LT(ms, 5000) << "the next request kept the 20 s timeout";
}

TEST_F(AlpacaRequestLimitsTest, NoLimitsChangeNothing)
{
    // PutAction without limits (every caller but ScopeAlpaca::Guide): the client's timeout, no
    // check, even with a stop requested
    g_stop = true;
    PutHandle put(m_server.Port(), 800);
    RequestRun run = Perform(put.Get(), nullptr, 800);
    EXPECT_EQ(run.res, CURLE_OPERATION_TIMEDOUT);
    EXPECT_FALSE(run.aborted);
    EXPECT_GE(run.ms, 700);
    EXPECT_LT(run.ms, 5000);
}

TEST_F(AlpacaRequestLimitsTest, TimeoutOnlyLimitsAddNoAbortCheck)
{
    // a timeout with no abort check: a requested stop does not give the request up
    g_stop = true;
    PutHandle put(m_server.Port(), 30000);
    AlpacaRequestLimits limits;
    limits.timeoutMs = 900;
    RequestRun run = Perform(put.Get(), &limits, 30000);
    EXPECT_EQ(run.res, CURLE_OPERATION_TIMEDOUT);
    EXPECT_FALSE(run.aborted);
    EXPECT_GE(run.ms, 800);
    EXPECT_LT(run.ms, 5000);
}
