# STM32 调试速查（寄存器 / 缓冲区 / DMA）

> 用途：不依赖调试器 GUI 和万用表，通过 ST-LINK 命令行直接检查 STM32 运行时状态。
> 工具链：STM32CubeCLT 1.22.0（路径见 [BuildAndFlash.md](BuildAndFlash.md)）
> 适用：STM32H7 系列（本文寄存器地址以 H723 为例）

---

## 1. 常用命令

```bash
CLI=/opt/ST/STM32CubeCLT_1.22.0/STM32CubeProgrammer/bin/STM32_Programmer_CLI
```

| 命令 | 用途 |
|---|---|
| `$CLI -l` | 列出 ST-LINK 探头（SN、串口） |
| `$CLI -c port=SWD mode=HOTPLUG -r32 <addr> <bytes>` | 读内存/寄存器（不复位） |
| `$CLI -c port=SWD mode=HOTPLUG -w32 <addr> <data>` | 写 32 位数据 |
| `$CLI -c port=SWD mode=HOTPLUG -halt` | 暂停内核 |
| `$CLI -c port=SWD mode=HOTPLUG -run` | 恢复运行 |
| `$CLI -c port=SWD -rst` | 复位系统 |
| `$CLI -c port=SWD -w <elf> -v -rst` | 烧录 ELF 并复位 |

### 输出解析注意

- `-r32` 的 `<bytes>` 是**字节数**（按 32 位对齐，最少读 4 字节）
- 每行输出形如 `0x24000010 : 0002AA55 ...`，**每行都带自己的地址前缀**
- 输出含 ANSI 颜色码，脚本处理前要 `re.sub(r'\x1b\[[0-9;]*m','',out)`
- `mode=HOTPLUG` 连接/断开可能让内核恢复运行；需要持续暂停时用 `-halt`，并在同一次调用里完成读取

---

## 2. H723 常用寄存器地址

### UART4（`0x40004C00`）

| 偏移 | 寄存器 | 关注位 |
|---|---|---|
| `+0x00` | CR1 | UE(0), RE(2), TE(3) → 正常值 `0x0D` |
| `+0x08` | CR3 | DMAR(6) RX DMA、DMAT(7) TX DMA、EIE(0) |
| `+0x1C` | ISR | PE(0) FE(1) NF(2) ORE(3) IDLE(4) RXNE(5) TC(6) TXE(7) |
| `+0x24` | RDR | 读会消费一个字节 |
| `+0x28` | TDR | 写一个字节即可发送（TE=1 时） |

> 空闲时 ISR 通常只有 TC/TXE = `0x000000C0`。
> `RXNE=1` 说明有字节没被取走；`ORE=1` 说明已经溢出。

### DMA1（`0x40020000`，Stream0 为例）

| 偏移 | 寄存器 | 关注点 |
|---|---|---|
| `+0x10` | S0CR | EN(0) 使能、DIR(6:7)、CIRC(8)、MINC(10) |
| `+0x14` | S0NDTR | 剩余传输数；数据到达时应递减 |
| `+0x18` | S0PAR | 外设地址（UART4_RX = `0x40004C24`） |
| `+0x1C` | S0M0AR | 目标内存地址 |
| `+0x24` | S0FCR | FIFO 配置 |

Stream n 的寄存器基址：`0x40020010 + 0x18 * n`（Stream0/1 在 DMA1）。

### DMAMUX1

| 地址 | 内容 |
|---|---|
| `0x40020800 + 0x4 * channel` | `CCR`，低 7 位 = 请求号 |

- `DMA_REQUEST_UART4_RX = 63`（`0x3F`）
- `DMA_REQUEST_UART4_TX = 64`

### GPIOA（`0x58020000`）

| 偏移 | 寄存器 | 用途 |
|---|---|---|
| `+0x00` | MODER | PA11/PA12 = `2` 表示复用模式 |
| `+0x10` | IDR | 读引脚实时电平（bit n = PAn） |
| `+0x24` | AFR[1] | PA8–PA15 复用号；`6` = UART4 |

### NVIC

| 地址 | 内容 |
|---|---|
| `0xE000E104` | ISER1（IRQ 32–63）；UART4 = IRQ 52 → bit20 |

---

## 3. 符号地址与结构体偏移

```bash
cd CodeMyCar/stm32_project
arm-none-eabi-nm build/Debug/stm32_project.elf | grep uart_dma_buffer
# 20000a50 ...
```

- `nm`: 静态变量/函数的地址
- `build/Debug/stm32_project.map`: 段布局与大小
- 结构体字段偏移（含对齐/填充）用 GDB 看：

```bash
arm-none-eabi-gdb -batch -ex 'ptype /o uart_dma_buffer_t' build/Debug/stm32_project.elf
```

示例（`uart_dma_buffer_t`，总大小 `0x5C0`）：

| 字段 | 偏移 |
|---|---|
| `rx_buffer[2][480]` | `0x000`（buffer0 `0x000`，buffer1 `0x1E0`） |
| `tx_buffer[480]` | `0x3C0` |
| `current` | `0x5A0` |
| `length` | `0x5A2` |
| `semaphore` | `0x5A4` |

> 注意 `__ALIGNED(32)` 会把整个结构体按 32 字节对齐/补齐，偏移不要手算错。

读取方法（把 `length` 和 `current` 一次读出）：

```
current/length 位于 base + 0x5A0 的一个 32 位字：
  current = val & 0xFF
  length  = (val >> 16) & 0xFFFF
```

---

## 4. UART 收不到数据：排查顺序

1. **对端自环**：让发送端 TX↔RX 短接，确认发送端本身正常。
   - ESP32：GPIO4↔GPIO5；STM32：PA12↔PA11。
2. **引脚配置**：`GPIOx_MODER`、`AFR`、`IDR`（空闲应为高）。
3. **UART 使能**：`CR1.UE/RE/TE`、`CR3.DMAR/DMAT`。
4. **DMA 流**：`CR.EN`、`NDTR` 是否递减、`PAR`/`M0AR` 是否正确。
5. **DMAMUX**：请求号是否等于外设请求（UART4_RX=63）。
6. **缓冲区地址**：是否在 **DTCM/ITCM**（DMA 不可访问）→ 见第 5 节。
7. **错误标志**：`ISR` 的 `ORE/FE/NE`；错误回调是否重新 arm。
8. **协议层**：帧头/长度/CRC、ACK 的 seq 在帧头还是 payload。

---

## 5. 内存区域与 DMA 可达性（H7）

| 区域 | 地址 | 大小（H723） | DMA1/2 可达 | D-Cache |
|---|---|---|---|---|
| ITCM | `0x00000000` | 64 KB | ❌ | 不经过 cache |
| DTCM | `0x20000000` | 128 KB | ❌ | 不经过 cache |
| AXI SRAM (RAM_D1) | `0x24000000` | 320 KB | ✅ | cacheable |
| D2 SRAM | `0x30000000` | 32 KB | ✅ | cacheable |
| D3 SRAM | `0x38000000` | 16 KB | ✅ | 视 MPU 配置 |

**规则**：

- DMA 源/目标缓冲区必须放 `RAM_D1` / D2 / D3，不能放 TCM。
- 放在 DTCM 的典型症状：DMA `NDTR` 不变、`RXNE/ORE` 置位、无传输错误标志。
- 本项目做法：链接脚本增加 `.dma_buffer` 段 → `>RAM_D1`，变量加 `section(".dma_buffer")`。

**Cache**：

- D-Cache 未开启 → 无需处理。
- 开启后：TX 前 `SCB_CleanDCache_by_Addr()`；RX 后 `SCB_InvalidateDCache_by_Addr()`。

> `.dma_buffer` 是 `NOLOAD` 段，启动不会清零；缓冲区自身需要显式初始化（`current/length/semaphore`）。

---

## 6. 读取并解析 RX 缓冲区（Python 片段）

```python
import re, subprocess
CLI = "/opt/ST/STM32CubeCLT_1.22.0/STM32CubeProgrammer/bin/STM32_Programmer_CLI"

def read_buf(addr, n=480):
    out = subprocess.run([CLI, "-c", "port=SWD", "mode=HOTPLUG",
                          "-r32", hex(addr), str(n)],
                         capture_output=True, text=True).stdout
    out = re.sub(r'\x1b\[[0-9;]*m', '', out)
    words = []
    for line in out.splitlines():
        m = re.match(r'\s*0x([0-9A-Fa-f]+)\s*:\s*(.*)', line)
        if m and addr <= int(m.group(1), 16) < addr + n:
            words += [int(w, 16) for w in m.group(2).split()]
    return b''.join(w.to_bytes(4, 'little') for w in words)

def frames(data):
    out, i = [], 0
    while True:
        j = data.find(b'\x55\xaa', i)
        if j < 0 or j + 6 > len(data): break
        ln = data[j + 5]
        if j + 8 + ln <= len(data):
            out.append(data[j:j + 8 + ln])
        i = j + 2
    return out

for base in (0x24000000, 0x240001E0):   # rx_buffer[0] / [1]
    for f in frames(read_buf(base)):
        print(f"type={f[2]} seq={f[3]} flags=0x{f[4]:02X} len={f[5]} "
              f"payload={' '.join(f'{b:02X}' for b in f[6:6+f[5]])}")
```

> 读之前先确认 DMA 当前指向哪个 buffer（`S0M0AR`），或两个都读。

---

## 7. 无万用表时的替代验证

| 目标 | 方法 |
|---|---|
| ESP32 UART 是否正常 | GPIO4↔GPIO5 自环 + 临时日志打印收到的帧 |
| STM32 UART RX 是否正常 | PA12↔PA11 自环 + 向 `UART4_TDR` 写一个字节，检查 `NDTR`/`length` |
| STM32 TX 是否正常 | 自环下发送完整 CONTROL 帧，应收到 ACK 帧（`length=8`） |
| 引脚是否有信号 | 读 `GPIOx_IDR` 采样（注意：浮空引脚也会随机跳变，不能单独作为结论） |

---

## 8. 相关文档

- [编译与烧录](BuildAndFlash.md)
- [ESP32 ↔ STM32 接线](HardwareWiring.md)
- [本次 UART4 DMA 故障的完整排查记录](UART-DMA-Troubleshooting.md)
