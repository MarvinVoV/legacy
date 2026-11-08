# CodeMyCar Drive Console

面向手机浏览器的 CodeMyCar 驾驶控制台，基于 Vue 3 + TypeScript + Vite。

## 功能

- 二维触控摇杆：纵向速度、横向转向，松手自动急停
- DRIVE / DIRECT 两种实时控制协议
- 精细、标准、运动三档速度上限
- 独立可靠 STOP：同时调用 L1 急停命令与 Agent STOP
- 设备在线状态、STM32 链路、实时速度、航向、前后超声波距离
- 相对距离/转角 GOAL 路径任务
- Pyserver 地址、设备和 API Key 可在页面内配置
- 页面隐藏、切换模式或控制链路异常时自动停车

## 本地启动

先启动 Mosquitto 和 pyserver，然后：

```bash
cd frontend
npm ci
npm run dev
```

开发服务器监听 `0.0.0.0:5173`。手机与电脑连接同一网络后，访问：

```text
http://<电脑局域网 IP>:5173
```

页面默认连接 `http://<当前页面主机>:8000`。如 pyserver 地址不同，点击右上角设置按钮修改。

## 控制语义

- 摇杆按下后约每 180 ms 发送一次命令，以续租 STM32 控制租约。
- 松手立即发送急停，不保留油门位置。
- 页面转向百分比范围为 `-100..100`，负数左转、正数右转；发送时按比例转换为 `curvature_1pm`（±3.77 1/m），适配当前 pyserver API。
- DRIVE 使用 `linear_vel + curvature_1pm`；DIRECT 使用左右相同 RPM + `curvature_1pm`，作为双轮直接控制模式。
- 速度档最大值分别为 250、500、800 mm/s；DIRECT 对应 75、150、240 RPM。

## 验证

```bash
npm run test:unit -- --run
npm run build
npx eslint .
CI=1 npm run test:e2e -- --project=chromium
```

生产构建输出到 `frontend/dist`。

访问 `http://127.0.0.1:5173/?mock=1` 可查看演示数据；演示模式不向真实车辆发送请求。浏览器测试使用该模式，与实车控制隔离。前端检查不能代替手机触控和真实停车链路验收。

## 后续验收 TODO（2026-11-08）

- 已完成：生产构建/类型检查、4 项控制转换单测、6,031 组生成请求通过当前 pyserver `ControlCommand` 模型校验。
- 待完成：Chromium mock 页面冒烟测试（本机缺少 Playwright 浏览器运行时，本轮不阻塞落地测试）。
- 待完成：手机触控、松手/切后台/断网停车、并发请求与 STOP 顺序、指令失败提示的实车端到端验收。完成前，落地测试优先使用已有的限时脚本和独立 STOP，不以新页面作为唯一停车手段。
