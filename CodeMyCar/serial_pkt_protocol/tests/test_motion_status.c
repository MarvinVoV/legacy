#include <assert.h>
#include <string.h>
#include "motion_status.h"

int main(void)
{
    _Static_assert(sizeof(motion_status_t) == MOTION_STATUS_SIZE,
                   "motion_status_t must be 36 bytes");

    motion_status_t s;
    memset(&s, 0, sizeof(s));
    s.seq = 17; s.state = 2; s.xMm = 861; s.yMm = -510; s.thetaMdeg = -29200;
    s.odometerMm = 994; s.goalDistanceMm = 994; s.goalTurnMdeg = -29200;
    s.goalDurationMs = 3412; s.leftMmps = -12; s.rightMmps = 13;

    uint8_t buf[MOTION_STATUS_SIZE];
    assert(motion_status_pack(&s, buf) == 0);
    assert(buf[0] == 17 && buf[1] == 2 && buf[2] == 0 && buf[3] == 0);
    assert(buf[4] == (uint8_t)(861 & 0xFF));  /* xMm little-endian */
    assert(buf[5] == (uint8_t)(861 >> 8));
    assert(buf[8] == 0x02);                   /* yMm = -510 -> 0xFFFFFE02 */
    assert(buf[9] == 0xFE);
    assert(buf[32] == 0xF4);                  /* leftMmps = -12 */
    assert(buf[33] == 0xFF);

    motion_status_t out;
    assert(motion_status_unpack(buf, sizeof(buf), &out) == 0);
    assert(out.seq == 17 && out.state == 2);
    assert(out.xMm == 861 && out.yMm == -510);
    assert(out.thetaMdeg == -29200);
    assert(out.goalTurnMdeg == -29200);
    assert(out.goalDurationMs == 3412);
    assert(out.leftMmps == -12 && out.rightMmps == 13);

    /* 长度不足必须拒绝 */
    assert(motion_status_unpack(buf, MOTION_STATUS_SIZE - 1, &out) != 0);
    return 0;
}
