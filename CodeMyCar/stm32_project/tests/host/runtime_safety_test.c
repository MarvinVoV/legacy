#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "control_lease.h"
#include "task_health.h"

int main(void)
{
    ControlLease lease;
    control_lease_init(&lease, 500u);
    assert(!control_lease_poll(&lease, 1000u));
    control_lease_renew(&lease, 1000u);
    assert(!control_lease_poll(&lease, 1500u));
    assert(control_lease_poll(&lease, 1501u));
    assert(lease.expired && !lease.active);

    /* uint32 tick wrap must not disable expiry. */
    control_lease_renew(&lease, UINT32_MAX - 100u);
    assert(!control_lease_poll(&lease, 300u));
    assert(control_lease_poll(&lease, 401u));
    control_lease_cancel(&lease);
    assert(!control_lease_poll(&lease, 5000u));
    assert(!lease.expired);

    const uint32_t required = (1u << TASK_HEALTH_MOTION) |
                              (1u << TASK_HEALTH_MOTOR_LEFT) |
                              (1u << TASK_HEALTH_MOTOR_RIGHT) |
                              (1u << TASK_HEALTH_CTRL) |
                              (1u << TASK_HEALTH_SERVO);
    TaskHealthMonitor health;
    task_health_init(&health, required, 100u, 1000u);
    assert(task_health_check(&health, 500u, 250u) == 0u); /* grace */
    for (unsigned i = 0u; i < TASK_HEALTH_COUNT; i++)
    {
        task_health_beat(&health, (TaskHealthId)i, 1050u);
    }
    assert(task_health_check(&health, 1200u, 250u) == 0u);
    task_health_beat(&health, TASK_HEALTH_MOTION, 1400u);
    task_health_beat(&health, TASK_HEALTH_MOTOR_LEFT, 1400u);
    task_health_beat(&health, TASK_HEALTH_MOTOR_RIGHT, 1400u);
    task_health_beat(&health, TASK_HEALTH_SERVO, 1400u);
    const uint32_t stale = task_health_check(&health, 1400u, 250u);
    assert(stale == (1u << TASK_HEALTH_CTRL));
    assert(health.faultLatched);
    assert(task_health_check(&health, 2000u, 250u) == stale);

    task_health_clear(&health, 2000u, 100u);
    assert(!health.faultLatched);
    assert(task_health_check(&health, 2050u, 250u) == 0u);

    printf("runtime_safety: all tests passed\n");
    return 0;
}
