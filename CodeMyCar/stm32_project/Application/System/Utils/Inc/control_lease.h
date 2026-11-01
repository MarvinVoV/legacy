#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    uint32_t renewedAtMs;
    uint32_t timeoutMs;
    bool active;
    bool expired;
} ControlLease;

void control_lease_init(ControlLease* lease, uint32_t timeoutMs);
void control_lease_renew(ControlLease* lease, uint32_t nowMs);
void control_lease_cancel(ControlLease* lease);
bool control_lease_poll(ControlLease* lease, uint32_t nowMs);
