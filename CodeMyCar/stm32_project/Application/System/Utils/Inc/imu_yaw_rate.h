#ifndef IMU_YAW_RATE_H
#define IMU_YAW_RATE_H
#include <stdbool.h>
#include <math.h>
#include <stddef.h>

/* Derivative of the same ZYX Euler yaw decoded from the sensor quaternion.
 * Body gyro Z alone is valid only when roll/pitch permit it. With normalized
 * quaternion, R32=sin(roll)cos(pitch), R33=cos(roll)cos(pitch), so
 * yaw_dot=(gyroY*R32+gyroZ*R33)/(R32^2+R33^2).
 * This is sensor-X yaw, not a sensor-to-chassis mounting calibration.
 * Reject near-vertical sensor X (|cos(pitch)|<0.2), where yaw is ill-conditioned.
 * Inputs/output rad/s; invalid inputs leave the output unchanged. */
static inline bool imu_euler_yaw_rate(float w, float x, float y, float z,
                                      float gyroY, float gyroZ, float* rate)
{
    if (rate==NULL || !isfinite(w) || !isfinite(x) || !isfinite(y) ||
        !isfinite(z) || !isfinite(gyroY) || !isfinite(gyroZ)) { return false; }
    const float norm=w*w+x*x+y*y+z*z;
    if (!isfinite(norm) || norm<0.5f || norm>1.5f) { return false; }
    const float inv=1.0f/sqrtf(norm);
    w*=inv; x*=inv; y*=inv; z*=inv;
    const float r32=2.0f*(y*z+w*x);
    const float r33=1.0f-2.0f*(x*x+y*y);
    const float cosPitch2=r32*r32+r33*r33;
    if (cosPitch2<0.04f) { return false; }
    const float result=(gyroY*r32+gyroZ*r33)/cosPitch2;
    if (!isfinite(result)) { return false; }
    *rate=result;
    return true;
}
#endif
