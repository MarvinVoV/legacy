#include "lighting_runtime.h"
#include "control_critical.h"

static LightHints s_hints;

void LightingRuntime_NewSession(uint32_t session)
{
    const uint32_t key=control_critical_enter();
    Lighting_NewSession(&s_hints,session);
    control_critical_exit(key);
}
bool LightingRuntime_Command(const uint8_t *payload,size_t length,uint32_t now)
{
    LightCommand command;
    const bool decoded=Lighting_UnpackCommand(payload,length,&command);
    const uint32_t key=control_critical_enter();
    bool accepted=false;
    if (decoded) {
        /* STM32-only reboot: first current-gateway hint can rebind the visual
         * channel without resetting motion seq windows or requiring ESP reset.
         * This trusted UART display channel is not an authorization boundary. */
        if (!s_hints.session) Lighting_NewSession(&s_hints,command.session);
        accepted=Lighting_Accept(&s_hints,&command,now);
    } else ++s_hints.rejected;
    control_critical_exit(key);
    return accepted;
}
void LightingRuntime_GetHints(LightHints *out)
{
    if (!out) return;
    const uint32_t key=control_critical_enter();
    *out=s_hints;
    control_critical_exit(key);
}
