/*
 *  alpaca_errors.cpp
 *  PHD Guiding
 *
 *  See alpaca_errors.h.
 */

#include "alpaca_errors.h"

#include <cstring>
#include <string>

#include "json_parser.h"

namespace AlpacaErrors
{
bool CaptureNeedsReconnect(long errorNumber)
{
    return errorNumber == kNotConnected;
}

CaptureFailureAction CaptureFailureTracker::OnFailure(long errorNumber)
{
    if (CaptureNeedsReconnect(errorNumber))
    {
        m_consecutive.store(0);
        return CaptureFailureAction::Reconnect;
    }
    if (errorNumber == kUnspecifiedError && m_consecutive.fetch_add(1) + 1 >= kAcquisitionFailuresBeforeReconnect)
    {
        m_consecutive.store(0);
        return CaptureFailureAction::Reconnect;
    }
    return CaptureFailureAction::Alert;
}

bool ReadError(const json_value *root, long *errorNumber, std::string *errorMessage)
{
    if (!root || root->type != JSON_OBJECT)
        return false;

    int number = 0;
    std::string message;
    json_for_each(n, root)
    {
        if (!n->name)
            continue;

        if (strcmp(n->name, "ErrorNumber") == 0)
        {
            if (n->type == JSON_INT)
                number = n->int_value;
            else if (n->type == JSON_FLOAT)
                number = static_cast<int>(n->float_value);
        }
        else if (strcmp(n->name, "ErrorMessage") == 0 && n->type == JSON_STRING)
        {
            message = n->string_value;
        }
    }

    if (errorNumber)
        *errorNumber = number;
    if (errorMessage)
        *errorMessage = message;

    return number != 0;
}
}
