#include <assert.h>
#include <stdio.h>

#include "encoder_fault_monitor.h"

int main(void)
{
    EncoderFaultMonitor m;
    encoder_fault_monitor_init(&m);

    assert(!encoder_fault_monitor_poll(&m, 0, 40, 0.3f, 0, 0, false));
    assert(!encoder_fault_monitor_poll(&m, 1500, 40, 0.3f, 0, 0, false));
    assert(encoder_fault_monitor_poll(&m, 1501, 40, 0.3f, 0, 0, false));

    encoder_fault_monitor_init(&m);
    assert(!encoder_fault_monitor_poll(&m, 0, 40, 0.3f, 0, 0, false));
    assert(!encoder_fault_monitor_poll(&m, 1000, 40, 0.3f, 0, 0.003f, false));
    assert(!encoder_fault_monitor_poll(&m, 2000, 40, 0.3f, 0, 0.006f, false));

    encoder_fault_monitor_init(&m);
    assert(!encoder_fault_monitor_poll(&m, 0, 40, 0.3f, 5, 0, false));
    assert(!encoder_fault_monitor_poll(&m, 2000, 40, 0.3f, 5, 0, false));

    encoder_fault_monitor_init(&m);
    assert(!encoder_fault_monitor_poll(&m, 0, 40, 0.05f, 0, 0, false));
    assert(!encoder_fault_monitor_poll(&m, 3000, 40, 0.05f, 0, 0, false));
    assert(!encoder_fault_monitor_poll(&m, 4000, 40, 0.3f, 0, 0, true));

    encoder_fault_monitor_init(&m);
    assert(!encoder_fault_monitor_poll(&m, UINT32_MAX - 1000u,
                                       40, 0.3f, 0, 0, false));
    assert(encoder_fault_monitor_poll(&m, 600u, 40, 0.3f, 0, 0, false));

    printf("encoder_fault_monitor: all tests passed\n");
    return 0;
}
