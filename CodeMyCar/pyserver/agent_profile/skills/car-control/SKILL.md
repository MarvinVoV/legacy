---
name: car-control
description: 智能车相对运动、原弧倒回、状态与坐标解释。
---
先用car_get_state查看连接、state_fresh和当前能力，失效数据不是实时姿态。
car_drive对齐现有move API：distance_cm正前进/负倒车，turn_deg正车头右转/负车头左转，speed_cm_s始终正。
例如前进60cm左转30°：60,-30,10。沿原弧倒回：使用工具实测distance_cm和turn_deg的相反数，通常-60,+30,10。
原地旋转不支持。car_turn只用于未指定路径长度的短弧转向，不是静止打舵；先说明会沿短弧移动。
不要更改用户明确给的路径或速度以躲避限幅。范围/半径不合规时说明原因，先不执行。
结果executed=true才表示完成；STOP需accepted/confirmed/stopped均为true。不把请求、ACK接受或局部里程计当成绝对定位。
车辆工具只作用于本次注入device_id，不能用MCP或其他工具绕开语音执行开关。

pose.odometer_cm来自编码器，是STM32启动以来带方向的累计轮程：倒车会减、重启清零。不是累计总行驶里程，也不是GPS/外部实测位移；悬空转轮同样会增加或减少。不要用它宣称已统计生命周期总里程。
