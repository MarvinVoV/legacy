# STM32H723 控制固件

STM32 执行运动控制和停车保护，采集 IMU、轮速、距离和电池数据，同时驱动六灯灯带与圆屏。日常开发使用当前 Release 预设。

在本目录执行以下命令。首次配置环境参见 [CubeCLT 安装路径与构建步骤](Docs/BuildAndFlash.md)。

```sh
cmake --preset Release && cmake --build --preset Release
bash tests/host/run.sh
```

外设实现与独立测试见 [STM32 文档](Docs/README.md)。跨板接线与整车验收见 [整车文档](../Docs/README.md)。
