# AmiiboZero — Cardputer Zero amiibo 模拟器技术设计方案

> **版本**：1.0  
> **编写日期**：2026-11-19  
> **目标硬件**：M5Stack Cardputer Zero + Cap CC1101（ST25R3916）  
> **技术方案**：Linux ARM64 · C++17 · CMake · LVGL · Linux SPI/GPIO  
> **状态**：待实施；**尚未证明当前硬件和 Linux 驱动可被 Nintendo Switch 成功识别**

---

## 1. 项目概述

AmiiboZero 是运行在 Cardputer Zero 上的独立 amiibo 模拟器。用户在设备上选择本地 amiibo 角色文件，程序使用 Cap CC1101 内的 **ST25R3916** NFC 芯片进行 13.56 MHz NFC-A 被动标签模拟，目标是向兼容读卡器呈现 **NTAG215** 行为，最终在支持 amiibo 的 Nintendo Switch 游戏中完成识别。

设备应在脱离电脑、ADB 和互联网后独立运行。**项目的核心难点是 NFC-A 被动模拟与 NTAG215 行为/响应时序，而不是 BIN 文件的读取。**

### 1.1 范围与目标

1. **NFC 开关与 amiibo 模拟**：在应用内提供独立的 NFC 开关。开启后，用户从列表中确认一个通过校验的角色 `.bin` 文件，应用才以该角色启动 amiibo 模拟。未选定角色时不对外响应。关闭 NFC 会停止当前模拟，此后不会响应读卡器。

- 自动发现本地 `.bin` 角色文件，显示中文文件名、游戏系列、UID 和文件有效性。
- 通过实体按键切换角色，一次只激活一个 NFC 模拟会话。
- 复用现有 ST25R3916 Linux SPI/GPIO 读卡工程，增加 NFC-A Listen（被动监听）通路。
- 构建独立于硬件的 NTAG215 内存模型与协议状态机。
- 提供射频事件、通信帧、超时及错误诊断，便于定位 Switch 无法识别的原因。
- 交付可重复构建的源码、测试与 ARM64 安装包。

### 1.2 非目标

- 不开发 CC1101 的 Sub-GHz 功能，项目只使用 Cap 上的 ST25R3916。
- 不修改 Switch 系统、游戏或游戏存档。
- 不提供任天堂专有密钥或角色 BIN 的公开分发功能。
- 不承诺任意第三方 BIN 都能被 Switch 识别。
- 不将“外部读卡器能读取 UID”作为最终成功标准。

---

## 2. 已有条件与待验证事项

### 2.1 已知设备与文件

| 项目 | 当前状态 |
|---|---|
| Cardputer Zero | Linux ARM64，ADB 可连接 |
| Cap CC1101 | 已配备；NFC 芯片为 ST25R3916 |
| amiibo 数据 | 已上传 25 个中文命名的塞尔达 `.bin`，单文件 540 字节 |
| 可选密钥 | `key_retail.bin`、`retail.bin`，各 160 字节，SHA-256 相同 |
| 设备用户目录 | `~/amiibo_sim/`（此前 ADB 查询 HOME 为 `/root`） |
| 设备文件总数 | 27 个 `.bin`，含 25 个角色文件和 2 个密钥文件 |
| 已验证事项 | 文件数量、文件大小、密钥哈希、ADB 文件清单 |

**注意**：已有资料只能证明文件已经就位，不能证明 NFC 模拟功能已经可用。原始 25 个角色文件在设备上的逐一哈希比对可作为实施前的补充验证。

### 2.2 关键未知量

1. ST25R3916 的 NFC-A Listen 模式在当前 BSP、SPI 和 IRQ 条件下是否可稳定启动。
2. 芯片硬件负责哪些帧级处理（防冲突、CRC、奇偶校验、帧时序），哪些必须由软件完成。
3. Linux 用户态 IRQ → SPI → 发送 FIFO 的延迟能否满足读卡器响应窗口。
4. 模拟标签的 UID、数据页、配置、签名/版本等行为能否匹配真实 NTAG215。
5. Nintendo Switch/游戏是否接受模拟的完整标签交互。

上述问题在实现前不能直接宣称“支持”。

---

## 3. 推荐技术架构

```text
┌──────────────────────────────────────────────┐
│ LVGL UI                                      │
│ 角色列表 / 中文搜索 / 详情 / 状态 / 日志       │
└───────────────────┬──────────────────────────┘
                    │ 应用事件队列
┌───────────────────▼──────────────────────────┐
│ Application                                  │
│ CatalogManager / SessionManager / Settings   │
└───────────┬───────────────────────┬──────────┘
            │                       │
    ┌───────▼──────────┐   ┌────────▼──────────┐
    │ BIN / Key Loader │   │ Ntag215Protocol   │
    │ 数据校验及索引    │   │ NTAG215 Memory    │
    └──────────────────┘   └────────┬──────────┘
                                   │ NFC 帧请求/响应
                          ┌────────▼──────────┐
                          │ NFC-A Listen      │
                          │ Adapter            │
                          └────────┬──────────┘
                                   │
                          ┌────────▼──────────┐
                          │ ST25R3916 Driver │
                          └────────┬──────────┘
                                   │
                          ┌────────▼──────────┐
                          │ Linux HAL         │
                          │ SPI / GPIO / IRQ  │
                          └────────┬──────────┘
                                   │
                          Cap CC1101 NFC 天线
                                   │
                             外部读卡器 / Switch
```

### 3.1 技术栈

| 层级 | 选型 | 理由 |
|---|---|---|
| 主程序 | C++17 | 低开销、直接复用现有 Linux NFC 代码 |
| 构建 | CMake + CTest | 与参考工程一致，便于主机端测试 |
| 图形 | LVGL | 适合设备小屏幕，已有同类工程基础 |
| 桌面模拟 | SDL2 | 不接设备即可完成 UI 和文件管理测试 |
| NFC 通信 | ST25R3916 + spidev + libgpiod | 复用已有硬件链路 |
| 日志 | spdlog 或轻量环形缓冲 | 高频通信路径无阻塞 |
| 配置 | JSON | 可读、易排障 |
| 部署 | ARM64 Debian 包、ADB | 适配 Linux/APPLaunch 工作流 |

### 3.2 线程模型

- **UI 主线程**：LVGL 渲染、按键与菜单。绝不在其中等待 NFC SPI 操作。
- **NFC Worker**：独占 SPI/IRQ 和协议状态机，以短路径处理帧。
- **文件/日志任务**：扫描目录与异步落盘；模拟中不得在 NFC 热路径进行文件读写。
- **消息传递**：有界队列（例如 `Start`, `Stop`, `Select`, `RfOn`, `FrameReceived`, `Error`）。

**设计约束**：协议引擎只接收抽象的帧，不直接依赖 LVGL、Linux 设备节点或 ST25R3916 寄存器。

---

## 4. 参考工程及复用方式

### 4.1 首选：CardputerZero/Cap-CC1101-NFC

仓库：<https://github.com/CardputerZero/Cap-CC1101-NFC>

该项目 README 公开的现有能力包括：ST25R3916 读卡驱动、Linux SPI/GPIO、中断与供电处理、NFC-A UID/ATQA/SAK 发现、Type 2 NDEF 读取、LVGL、SDL mock、CMake/CTest 和 Debian 打包。

推荐复用范围：

- `src/hal/`：SPI/GPIO、扩展供电、显示及环境初始化。
- `src/nfc/st25r3916_driver.*`：底层寄存器、FIFO、设备控制，可扩展 Listen 功能。
- `src/nfc/nfc_backend.*`、`nfc_worker.*`：后端抽象及状态通知思路。
- `src/input/`、`src/views/`：按键及屏幕模式。
- `packaging/`：安装包脚手架。

**不能直接复用的部分**：其公开 README 描述的是**读卡器**能力，不是完整 NTAG215 被动标签模拟。需要新增独立 `ListenAdapter` 与模拟协议引擎。

### 4.2 辅助参考

- <https://github.com/mtoolstec/CardputerZero-RFID>：参考 NFC CAP 的 Listener / NTAG 相关代码与能力边界。
- <https://github.com/BruceDevices/firmware>：参考 NFC-A、NTAG21x 行为与模拟状态机。该项目的固件环境与 Linux 不同，不能整包移植。
- ST 的 ST25R3916 数据手册及 RFAL 资料、NXP 的 NTAG213/215/216 数据手册：作为最终寄存器与协议依据。

第三方实现必须审核许可、版本及适用硬件；项目 README 中的声明不等于已完成本机实测。

---

## 5. 硬件驱动与实时性

### 5.1 当前参考配置

| 资源 | 参考值 | 使用说明 |
|---|---|---|
| SPI 设备 | `/dev/spidev0.2` | 以设备实际配置为准 |
| SPI CS | GPIO22 | 由内核管理的片选 |
| NFC IRQ | GPIO23 | 接收事件与中断 |
| POWER_EN | GPIO26 | 与扩展供电相关 |
| 电源 | EXT5V | 可能与 LCD 通路存在共享依赖 |

这些值来自参考工程 README。开发时先验证设备树、SPI 节点、IRQ、驱动实际配置；不要盲目改动系统设备树。

### 5.2 Listen 模式启动顺序

1. 验证 SPI/GPIO 可访问且 NFC 设备未被其他应用占用。
2. 检查 ST25R3916 芯片 ID、复位及中断状态。
3. 根据芯片数据手册配置 **NFC-A passive target / listen** 所需的寄存器与中断。
4. 验证外部读卡器建立 RF 场时能收到事件。
5. 将接收 FIFO 帧解析为统一帧事件，交给协议引擎。
6. 将响应帧写入 TX FIFO，按协议及时发送。
7. 处理场消失、错误帧、超时、中断丢失和重新启动。
8. 退出时关闭 NFC 通信、释放文件描述符和 GPIO 资源。

**特别注意**：根据参考工程，GPIO26 POWER_EN/EXT5V 供电可能与 LCD 共用；关闭应用时不能直接切断共享供电，否则可能导致设备屏幕冻结。

### 5.3 时序指标

对每种 NFC 响应记录：IRQ 触发时间、读取 FIFO 完成、协议响应完成、写入 TX FIFO、开始发送。统计 P50/P95/P99 和最坏值，按真实 Type 2 / ISO14443-A 协议时序评估。

优先使用芯片硬件支持的自动响应能力。若用户态调度达不到时序，再考虑更严格的线程调度、减少系统负载，或评估其他底层实现；不要直接承诺“Linux 用户态一定能模拟”。

---

## 6. 文件管理、密钥与数据模型

### 6.1 文件识别

默认扫描设备 `~/amiibo_sim/`，后续可选递归扫描子目录。角色文件和密钥文件必须分开处理。

| 类型 | 识别规则 | UI 行为 |
|---|---|---|
| 角色原始 BIN | 预期大小 540 字节 + 结构校验 | 展示为可选角色 |
| `key_retail.bin` | 已知密钥文件名，预期 160 字节 | 仅在设置中显示存在状态 |
| `retail.bin` | 同类备用文件名，预期 160 字节 | 不计入角色数 |
| 其他文件 | 不匹配或校验失败 | 忽略并记录原因 |

原始文件不因名称为 `.bin` 就被视为有效。显示名称可按 `游戏系列－角色名.bin` 提取；例如 `王国之泪－林克.bin`。提取失败时直接显示原文件名。

角色索引字段建议：`path`、`displayName`、`series`、`uid`、`size`、`sha256`、`validationStatus`、`lastUsedAt`。使用内容哈希识别重复文件，但允许同名不同内容出现“冲突待处理”。

### 6.2 NTAG215 540 字节模型

常见 amiibo 原始转储是 **540 字节 = 135 页 × 4 字节**。协议层应建模以下部分：

- UID / BCC 和静态锁定区；
- Capability Container；
- 用户与应用数据；
- 动态锁定、配置、认证及其他尾部区域；
- 独立于 540 字节文件的版本、原厂签名、计数器等设备行为（若适用）。

精确页地址、保护区、PWD_AUTH 行为和特殊寄存器必须严格依据 NXP NTAG215 文档，**不要把存储映像等同于芯片所有行为**。

建议接口：

```cpp
class Ntag215Memory {
public:
    bool load(const std::array<uint8_t, 540>& image);
    Result readPages(uint8_t firstPage, uint8_t count) const;
    Result writePage(uint8_t page, std::span<const uint8_t, 4> data);
    void resetRfSession();
    std::array<uint8_t, 540> snapshot() const;
};
```

上述接口是**概念性伪代码**，若坚持 C++17，应将 `std::span` 替换为指针/长度或 `std::array<uint8_t, 4>`；实现不得依赖 C++20 API。

### 6.3 原始文件保护与写入

默认只读原始 BIN。进入模拟时复制为内存快照；写入操作仅修改快照。后续提供“保存会话”选项，另建版本化快照文件并记录 SHA-256；默认不覆盖原始文件。

### 6.4 密钥管理

- **首次 MVP 不要求必须加载密钥**：直接提供完整的原始标签数据时，是否需要解密取决于产品特性。
- `key_retail.bin` 与 `retail.bin` 不应都作为独立密钥对象使用；相同哈希时视为同一份内容。
- 密钥只在必要的解析、校验或重新计算数据功能中启用。
- 不上传到公开 Git 仓库、不打印到日志、不编译进可执行程序、不经网络传输。
- 使用有权使用的数据和密钥；第三方仓库提供文件不自动意味着拥有分发授权。

---

## 7. NFC-A 与 NTAG215 协议实现

### 7.1 模块拆分

**NfcAListenAdapter**：负责 NFC-A 防冲突相关的帧级能力与芯片适配，屏蔽 ST25R3916 自动处理能力和软件处理范围。

**Ntag215ProtocolEngine**：负责 Type 2 标签命令的业务语义，读取 `Ntag215Memory`，生成应答或错误。

**Ntag215Memory**：负责按页读取、写保护、锁定位、认证相关状态，以及模拟快照。

### 7.2 发现与选中

1. REQA / WUPA 唤醒和标签发现。
2. 按目标标签要求发送 ATQA。
3. 7 字节 UID 的两级防冲突（Cascade Level）。
4. 正确处理 BCC、SELECT 与 SAK。
5. 转入 ACTIVE 状态，接受 Type 2 Tag 命令。
6. HALT、场消失和新一轮唤醒时正确重置状态。

必须检查 ST25R3916 是否自动处理部分 CRC、奇偶位或防冲突过程，避免同时在硬件和软件中执行。

### 7.3 Type 2 Tag 命令表

| 命令/行为 | 功能 | 验证重点 |
|---|---|---|
| `READ` | 读取连续四页 | 访问范围与页边界 |
| `FAST_READ` | 读取指定页区间 | 长度、起止页及异常范围 |
| `GET_VERSION` | 标签版本 | 返回数据是否与 NTAG215 匹配 |
| `READ_SIG` | 读取原厂签名 | 未具备真实材料时不得伪称认证真实 |
| `WRITE` | 写入页面 | 锁定位、权限、ACK/NAK |
| `PWD_AUTH` | 标签密码认证 | 授权状态、失败行为 |
| `READ_CNT` | 读取计数器（若配置支持） | 计数器状态与协议细节 |
| `HALT` | 停止会话 | 再唤醒行为与状态复位 |

应依据 NXP 手册再确认芯片版本所需指令及命令码，而不是机械复制表格。所有请求都要做输入长度、CRC、状态、地址合法性校验；非法请求不能引起越界访问或应用崩溃。

### 7.4 协议状态机

```text
OFF ─▶ IDLE ─▶ READY ─▶ ACTIVE
          ▲                 │
          └──── WUPA ◀─ HALT

错误、超时、RF 场丢失 → 会话重置
```

状态机单元测试应覆盖正常过程、错误帧、重复命令、RF 丢失、读写后重新扫描。

---

## 8. UI / UX 与交互

主流程：**开启 NFC → 确认角色 BIN → 等待读卡器 → 关闭 NFC**。开关状态和模拟状态分别显示；仅开启 NFC 不代表模拟已经开始。

```text
AMIIBO ZERO
─────────────────────────
NFC：开（待选角色）
王国之泪  [5]
> 林克
  塞尔达
  加侬多夫
  ……
─────────────────────────
Enter 确认角色   长按 Esc 3 秒退出
```

### 8.1 页面

- **角色列表**：提供 NFC 开关，显示开关状态、系列分组、中文名称、有效/无效标记、最近使用。
- **角色详情**：系列、文件名、UID、540 字节验证、内容哈希短码。
- **模拟页面**：可关闭 NFC，显示会话状态、当前角色、检测到的 RF 场、通信次数、最近错误。
- **设置/诊断**：数据目录、NFC 驱动状态、可选密钥检测、日志开关、版本。

### 8.2 按键

- **Z 左、X 下、C 右、F 上**：移动焦点；在角色列表中，上/下切换条目，左/右切换系列。
- **Enter**：确认当前角色，只有 NFC 已开启且 BIN 校验通过时才可启动模拟。
- **连续长按 Esc 3 秒**：退出应用，返回 APPLaunch；提前松开取消退出。退出时应停止模拟并释放硬件。关闭 NFC 使用独立开关。

启动时默认关闭 NFC 模拟，Enter 开启功能并直接进入角色列表。F/X 逐行移动，Z/C 翻页，N 随时关闭。列表优先高亮上次确认且仍有效的 BIN；只有用户再次按 Enter 才重新校验并启动被动监听。启动失败显示错误。退出不保存开关状态，只记录最后确认的文件名。

### 8.3 运行状态

NFC 开关是应用内是否允许模拟的设置，不代表硬件或会话已就绪。会话状态独立为 `STOPPED` → `INITIALIZING` → `WAITING_FOR_FIELD` → `SELECTED` → `COMMUNICATING` → `STOPPING` → `STOPPED`，异常进入 `ERROR`。不得把“已进入 Listen 模式”显示成“Switch 已识别成功”。

---

## 9. 会话管理、配置与日志

### 9.1 会话规则

- 同时只允许一个活动角色。
- NFC 开启不自动启动模拟；关闭时停止活动会话、退出 Listen 模式并释放 SPI/GPIO 资源。
- 启动前检查 BIN、构建内存快照、确认 SPI/GPIO 未被其他进程占用。
- 切换角色必须先停止当前 RF 会话，再替换数据。
- 异常后先释放驱动资源再允许重试；设备异常不能使 UI 永久卡死。
- 接收写入时更新当前快照；原始文件不变。

### 9.2 配置建议

```json
{
  "amiibo_directory": "~/amiibo_sim",
  "ui_language": "zh-CN",
  "log_level": "INFO",
  "save_session": false,
  "auto_restore_last_selection": true
}
```

路径中的 `~` 需在应用启动时展开为 HOME。配置写入采用临时文件和原子替换；**不要把密钥原文放进 JSON**。

### 9.3 日志与测量

- INFO：角色选中、会话开始/停止、RF 场变化。
- WARN：无效帧、认证失败、响应过慢、文件异常。
- ERROR：设备初始化失败、SPI/IRQ 错误及不可恢复状态。
- DEBUG/TRACE：收发帧摘要、指令处理时间和底层诊断，仅调试时开启。

建议记录：IRQ 数、RX/TX 帧数、CRC 错误、超时、FIFO 错误、P95/P99 延迟、重新初始化次数。日志采用限长环形缓冲，避免 NFC 热路径阻塞。

---

## 10. 工程目录

```text
cardputer-amiibo-simulator/
├── README.md
├── TECHNICAL_DESIGN.md
├── CMakeLists.txt
├── .gitignore
├── cmake/
├── include/
├── src/
│   ├── main.cpp
│   ├── app/
│   ├── catalog/
│   ├── amiibo/
│   │   ├── bin_loader.cpp
│   │   ├── ntag215_memory.cpp
│   │   └── ntag215_protocol.cpp
│   ├── nfc/
│   │   ├── listen_adapter.cpp
│   │   └── st25r3916_driver.cpp
│   ├── hal/
│   └── ui/
├── tests/
│   ├── bin_loader_test.cpp
│   ├── ntag215_memory_test.cpp
│   ├── ntag215_protocol_test.cpp
│   └── session_manager_test.cpp
├── assets/
├── docs/
└── packaging/
```

依赖固定版本。`.gitignore` 必须排除真实 `.bin` 转储、密钥文件、日志、缓存和构建产物；测试中使用自行构造的非专有数据样本。

---

## 11. 构建与部署

设备端参考 [CardputerZero/Template](https://github.com/CardputerZero/Template) 的 LVGL、Linux framebuffer 和 evdev 接入方式。按 [APPLaunch 打包指南](https://github.com/CardputerZero/launcher/blob/master/docs/APPLaunch-App-打包指南.md) 安装 `AmiiboZero` 的 `.desktop` 入口与自绘 NFC 角色卡 PNG 图标；通过 `Terminal=false` 从应用列表启动，进程退出后由 APPLaunch 恢复主界面。

### 11.1 开发策略

1. 先建立纯协议 C++17 库以及 CTest 单元测试。
2. 建立 SDL mock UI 与模拟帧测试，不接硬件即可运行。
3. 复用参考工程的 SPI/GPIO 代码，接入 Listen Adapter。
4. 设备端以开发模式运行，使用 ADB 观察日志。
5. 在真实 NFC 读卡器上完成协议验收后，进行 Switch 游戏测试。
6. 最后按参考工程现有方式生成适配 Cardputer Zero 的 ARM64 安装包。

### 11.2 部署原则

- 在桌面源码目录内构建，不修改设备系统关键文件作为首选方案。
- 优先使用参考工程已有的打包与 APPLaunch 集成路径。
- ADB 只用于开发、安装、诊断；用户正式使用不要求连接电脑。
- 安装包不得包含任天堂专有密钥或角色数据；数据由使用者自行准备。

---

## 12. 测试计划与验收门槛

| 阶段 | 测试项目 | 最低通过标准 |
|---|---|---|
| M1 文件系统 | 扫描中文文件、分类、完整性校验 | 正确识别 25 个角色并排除 2 个密钥 |
| M2 协议单测 | READ、FAST_READ、写保护、非法输入 | 自动化用例全通过，不崩溃不越界 |
| M3 开关与选角 | 开启 NFC、确认有效或无效 BIN、模拟中关闭 NFC | 未确认有效 BIN 时不启动模拟；确认有效 BIN 后等待读卡器；关闭后不再响应 |
| M3 NFC-A 基础 | 场检测、UID 防冲突、SELECT | 外部读卡器能稳定完成标签选择 |
| M4 NTAG215 | 数据页、版本、错误和状态行为 | 读卡器读取的数据与测试映像一致 |
| M5 Switch | 游戏内识别及重复扫描 | 至少多个不同系列角色成功识别 |
| M6 稳定性 | 多次停止、重启、切换及异常恢复 | 无死锁、无屏幕冻结、无原始文件破坏 |
| M7 交付 | 独立运行、打包、文档 | 可复现构建，可在设备安装运行 |

### 12.1 必测异常

- 无 Cap 模块、SPI 节点缺失、设备被另一个进程占用。
- 无可用 BIN、文件仅有密钥、文件大小错误、文件名包含中文字符。
- 读卡器在防冲突期间移走、重复唤醒、非法页地址、CRC 错误。
- 游戏读卡中停止或切换角色。
- 启停 100 次、连续扫描、多次 RF 场丢失及重建。

### 12.2 最终成功定义

**成功 = 在 Cardputer Zero 独立运行时，用户选定角色，Switch 游戏实际识别对应 amiibo，重复扫描稳定且不会损坏原始数据。**

尚未实现 M3/M4 时不得宣称“amiibo 模拟成功”。

---

## 13. 风险、决策点与替代路线

| 风险 | 影响 | 应对策略 |
|---|---|---|
| ST25R3916 的当前驱动缺少 Type 2 Listener | 无法完成模拟 | 优先验证 Listen 底层，再扩展协议 |
| Linux 用户态响应不够及时 | 读卡器反复超时 | 延迟测量、硬件自动响应、优化调度 |
| 540 字节 BIN 不涵盖全部标签行为 | Switch 拒绝识别 | 逐项验证签名/版本/计数器等行为 |
| RF 场/天线耦合不稳定 | 近距离识别失败 | 固定位置、分析射频事件和读卡统计 |
| 共享供电影响 LCD | 界面冻结 | 参考成熟驱动，退出只关闭 NFC 工作状态 |
| 第三方数据或密钥的授权问题 | 无法公开分发 | 仓库仅保存源码和非专有测试样本 |

### 13.1 停止线（Go/No-Go）

- **Go**：外部读卡器能稳定完成防冲突和标准数据页读取，且时序具备余量，可继续做 Switch 兼容。
- **No-Go**：当前 ST25R3916/Linux 组合在调试后仍无法满足 Type 2 Tag 时序。暂停 UI 深化，先评估现有驱动、芯片能力或替代的 NFC 模拟硬件，不继续堆砌应用功能。

### 13.2 备用技术方案

如果 Cap CC1101 的 ST25R3916 在当前平台上无法可靠完成模拟，保留 UI、BIN Loader、NTAG215 引擎与会话管理，通过新 `NfcBackend` 适配其他**已证明支持 NTAG215 模拟**的硬件；无需重写整个应用。

---

## 14. 建议实施顺序

**第 0 步：基础审计**。检查设备实际硬件节点、内核/BSP、既有 NFC 应用是否占用设备；审查参考工程许可证、构建命令和可复用类。

**第 1 步：文件 MVP**。完成角色索引、中文列表、540 字节校验、密钥过滤及 SDL Mock UI。应先交付可运行的列表程序。

**第 2 步：协议 MVP**。实现 NTAG215 内存/Type 2 协议纯逻辑和高覆盖率测试，不接真实 ST25R3916。

**第 3 步：硬件最小验证**。实现 NFC-A Listen 下的 RF 场检测、UID 防冲突和 SELECT，验证真实读卡器能选中目标标签。

**第 4 步：完整模拟**。实现 READ/FAST_READ/权限/状态行为，测量时序，和真实 NTAG215 对比。

**第 5 步：Switch 验收**。在支持 amiibo 的游戏中测试多个角色，保存成功/失败记录，修复差异。

**第 6 步：正式交付**。完善 LVGL、日志、安装包、异常恢复及说明文档。

建议尽早投入“第 3 步硬件验证”：这是整个项目最大的可行性门槛。

---

## 15. 参考资料

1. CardputerZero/Cap-CC1101-NFC：<https://github.com/CardputerZero/Cap-CC1101-NFC>
2. CardputerZero-RFID：<https://github.com/mtoolstec/CardputerZero-RFID>
3. Bruce Firmware：<https://github.com/BruceDevices/firmware>
4. M5Stack 官方产品/硬件资料：<https://docs.m5stack.com/>
5. STMicroelectronics ST25R3916 产品与数据手册：<https://www.st.com/>
6. NXP NTAG213/215/216 产品数据手册：<https://www.nxp.com/>

> 文档说明：本方案描述开发路径及验收方法。上述部分硬件能力尚未在目标设备上实测；文中参考接口以目前公开参考工程为依据，落地时需再次核对源代码与设备环境。
