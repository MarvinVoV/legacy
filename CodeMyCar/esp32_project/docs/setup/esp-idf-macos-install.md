# macOS 安装 ESP-IDF（通过 EIM）

> 来源：[Espressif 官方文档](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/macos-setup.html)
> 适用版本：ESP-IDF v6.x（当前安装 **v6.1.0**；v6.0 之前的旧方法见 [legacy 文档](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/linux-macos-setup-legacy.html)）

---

## 概述

ESP-IDF v6.0 起，官方推荐使用 **EIM（Espressif Installation Manager）** 来安装和管理 ESP-IDF。
EIM 替代了旧的手动流程（`git clone` → `install.sh` → `source export.sh`），一条命令完成全部安装。

## Step 1：安装前置依赖

通过 [Homebrew](https://brew.sh/) 安装：

```bash
brew install libgcrypt glib pixman sdl2 libslirp dfu-util cmake python
```

> **注意：** ESP-IDF 要求 Python ≥ 3.10。

## Step 2：安装 EIM

### 方式 A：Homebrew（推荐，便于后续更新）

```bash
brew tap espressif/eim

# CLI 版本
brew install eim

# 或 GUI 版本
brew install --cask eim-gui
```

### 方式 B：直接下载

从 <https://dl.espressif.com/dl/eim/> 下载在线/离线安装包（GUI 或 CLI）。

## Step 3：安装 ESP-IDF

### 方式 1：EIM CLI 在线安装（本项目使用）

```bash
# 非交互模式，安装最新稳定版（默认设置）
eim install

# 交互向导，可自定义路径、版本等
eim wizard

# 安装指定版本（例如 v5.4.2）
eim install -i v5.4.2

# 查看所有选项
eim --help
```

安装成功后终端输出：

```
INFO - Successfully installed IDF
INFO - Now you can start using IDF tools
```

### 方式 2：EIM GUI 在线安装

1. 打开 EIM 应用
2. `New Installation` → `Start Installation`
3. `Easy Installation` → `Start Easy Installation`（默认最新稳定版）
4. 如需自定义版本/路径，选择 `Custom Installation`

### 方式 3：配置文件安装

安装时会自动生成 `eim_config.toml`，可复制到其他电脑复现相同配置。
详见 [EIM 文档 > Configuration Files](https://docs.espressif.com/projects/idf-im-ui/en/latest/gui_configuration.html#configuration-files)。

### 方式 4：离线安装

从本地包安装，无需联网。
详见 [EIM 文档 > Offline Installation](https://docs.espressif.com/projects/idf-im-ui/en/latest/offline_installation.html)。

## 安装后：激活环境

每次打开新终端需要先激活（脚本名随安装版本变化，当前为 v6.1）：

```bash
source "/Users/sunyameng/.espressif/tools/activate_idf_v6.1.sh"
```

本机 `~/.zshrc` 已配置 `get_idf` 函数，日常可直接：

```bash
get_idf    # source 最近安装的 activate_idf_*.sh
```

> 该脚本由 EIM 自动生成，激活后 `idf.py` 等命令才可用。
> 可用 `ls ~/.espressif/tools/activate_idf_*.sh` 查看本机已安装版本的激活脚本。
> 关闭终端后环境变量失效，需重新 source / `get_idf`。
> shell 函数仅在交互式 shell 存在，脚本 / Makefile / CI 中请写完整 `source`。

激活后核实：

```bash
idf.py --version      # ESP-IDF v6.1
echo $IDF_PATH        # /Users/sunyameng/.espressif/v6.1/esp-idf
```

> ⚠️ 从旧版本升级后，项目里已有的 `build/` 仍指向旧 IDF 路径，先执行一次 `idf.py fullclean` 再编译。
> 环境变量完整清单见 [`toolchain-inventory.md`](toolchain-inventory.md)。

## 后续步骤

→ 参考 [Build Your First Project](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/index.html#get-started-build)

## EIM vs idf.py 的关系

| | EIM | idf.py |
|---|---|---|
| 定位 | 安装管理器（装环境） | 构建工具（日常开发） |
| 使用时机 | 装/切换 ESP-IDF 版本时 | 每天 build / flash / monitor |
| 包含关系 | EIM 安装 ESP-IDF → ESP-IDF 包含 idf.py | — |
