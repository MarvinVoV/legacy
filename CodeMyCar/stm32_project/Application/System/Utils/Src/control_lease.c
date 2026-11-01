#include "control_lease.h"

#include <stddef.h>

void control_lease_init(ControlLease* lease, const uint32_t timeoutMs)
{
    if (lease == NULL)
    {
        return;
    }
    lease->renewedAtMs = 0u;
    lease->timeoutMs = timeoutMs;
    lease->active = false;
    lease->expired = false;
}

void control_lease_renew(ControlLease* lease, const uint32_t nowMs)
{
    if (lease == NULL)
    {
        return;
    }
    lease->renewedAtMs = nowMs;
    lease->active = true;
    lease->expired = false;
}

void control_lease_cancel(ControlLease* lease)
{
    if (lease == NULL)
    {
        return;
    }
    lease->active = false;
    lease->expired = false;
}

bool control_lease_poll(ControlLease* lease, const uint32_t nowMs)
{
    if (lease == NULL || !lease->active || lease->timeoutMs == 0u)
    {
        return false;
    }
    if ((uint32_t)(nowMs - lease->renewedAtMs) <= lease->timeoutMs)
    {
        return false;
    }
    lease->active = false;
    lease->expired = true;
    return true;
}
