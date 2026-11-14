#include <assert.h>
#include <stdio.h>
#include <math.h>

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

    /* Field regression: 50mm/s is ~14.691RPM on a 32.5mm wheel.
     * Accepted DRIVE commands must not hide a powered, motionless wheel. */
    const float low_commands[] = {14.691f, -14.691f, 0.294f, -0.294f};
    for (unsigned i = 0; i < sizeof(low_commands)/sizeof(low_commands[0]); ++i)
    {
        encoder_fault_monitor_init(&m);
        assert(!encoder_fault_monitor_poll(&m, 0, low_commands[i], 0.2f, 0, 0, false));
        assert(!encoder_fault_monitor_poll(&m, 1500, low_commands[i], 0.2f, 0, 0, false));
        assert(encoder_fault_monitor_poll(&m, 1501, low_commands[i], 0.2f, 0, 0, false));
    }

    /* The same slow command with quantized encoder progress is healthy.
     * Exercise both directions, including the 1mm/s API resolution. */
    for (unsigned i = 0; i < sizeof(low_commands)/sizeof(low_commands[0]); ++i)
    {
        encoder_fault_monitor_init(&m);
        for (uint32_t t = 0; t <= 10000; t += 5)
        {
            float revs = truncf(low_commands[i] * (float)t / 60000.0f * 1560.0f) / 1560.0f;
            assert(!encoder_fault_monitor_poll(&m, t, low_commands[i], 0.2f, low_commands[i], revs, false));
        }
    }

    /* Genuine zero target, low effort and explicit braking remain excluded. */
    encoder_fault_monitor_init(&m);
    assert(!encoder_fault_monitor_poll(&m, 0, 0, 0.2f, 0, 0, false));
    assert(!encoder_fault_monitor_poll(&m, 10000, 0, 0.2f, 0, 0, false));
    assert(!encoder_fault_monitor_poll(&m, 11000, 14.691f, 0.05f, 0, 0, false));
    assert(!encoder_fault_monitor_poll(&m, 12000, 14.691f, 0.2f, 0, 0, true));

    /* Slow real motion at the near-zero boundary must tolerate quantization. */
    encoder_fault_monitor_init(&m);
    for (uint32_t t = 0; t <= 10000; t += 5)
    {
        float revs = truncf(0.1f * (float)t / 60000.0f * 1560.0f) / 1560.0f;
        assert(!encoder_fault_monitor_poll(&m, t, 0.1f, 0.2f, 0.1f, revs, false));
    }
    encoder_fault_monitor_init(&m);
    assert(!encoder_fault_monitor_poll(&m, 0, 0.1f, 0.2f, 0, 0, false));
    assert(!encoder_fault_monitor_poll(&m, 2000, 0.1f, 0.2f, 0, 0, false));
    assert(encoder_fault_monitor_poll(&m, 2500, 0.1f, 0.2f, 0, 0, false));

    printf("encoder_fault_monitor: all tests passed\n");
    return 0;
}
