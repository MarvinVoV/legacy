"""运行期设备配置（目前只有超声波开关）。"""

from pydantic import BaseModel, ConfigDict, StrictBool


class SonarConfigRequest(BaseModel):
    """超声波运行期开关。

    STM32 上电默认开启（安全优先）；关闭后不测距、不上报，
    安全限速完全放行（scale=1.0）。设备重启会恢复为开启。
    """

    model_config = ConfigDict(extra="forbid")
    enabled: StrictBool
