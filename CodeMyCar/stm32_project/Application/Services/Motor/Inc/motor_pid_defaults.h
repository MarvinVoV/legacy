#ifndef MOTOR_PID_DEFAULTS_H
#define MOTOR_PID_DEFAULTS_H

#ifndef CMCAR_MOTOR_PID_FINE_INTEGRATION
#define CMCAR_MOTOR_PID_FINE_INTEGRATION 0
#endif
/* Inputs are RPM / maxRPM. The fine-integration candidate closes the gap
 * above the measurement dead zone in a plant model, but hardware A/B did not
 * establish a repeatable improvement. Preserve baseline defaults. */
#define MOTOR_PID_DEFAULT_PARAMS { \
    .kp = 0.50f, .ki = 0.60f, .kd = 0.03f, .output_max = 1.0f, \
    .integral_threshold = CMCAR_MOTOR_PID_FINE_INTEGRATION ? 0.0001f : 0.005f, \
    .dead_zone = 0.002f \
}
#endif
