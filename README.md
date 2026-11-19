# AmiiboZero

面向 M5Stack Cardputer Zero 的 amiibo 模拟器项目，详见[技术设计](./TECHNICAL_DESIGN.md)。设备版已实现使用本地组合密钥校验、重签与自动轮换 UID。已有 Switch 游戏奖励实测成功反馈，连续扫描稳定性仍在修复。

## 本地构建与测试

需要 CMake 3.16 或更新版本、OpenSSL 开发库，以及支持 C++17 的编译器。不需要连接设备，也不需要角色数据或密钥。

```sh
cmake -S . -B build
cmake --build build
./build/amiibozero
(cd build && ctest --output-on-failure)
```

扫描本地角色或单独加载一个 BIN：

```sh
./build/amiibozero --list tests/local-fixtures
./build/amiibozero --inspect 'tests/local-fixtures/王国之泪－林克.bin'
```

扫描不进入子目录，并排除两种常见密钥文件名。校验仅检查 540 字节长度和 UID 校验字节等基础结构，不验证内容真实性或 Switch 兼容性。

## 可选本地测试样本

手动测试用的角色 BIN 和密钥可放在 `tests/local-fixtures/`，该目录已被 Git 忽略。默认的 CTest 不读取这些文件；自动化测试应使用合成样本。

不要提交或分发真实的 BIN 转储和密钥，也不要提交日志。

## Cardputer Zero 应用

设备版参考 [CardputerZero 官方模板](https://github.com/CardputerZero/Template) 的 LVGL / Linux framebuffer / evdev 架构，并按 [APPLaunch 打包指南](https://github.com/CardputerZero/launcher/blob/master/docs/APPLaunch-App-打包指南.md) 注册。安装后，在 APPLaunch 中打开 **AmiiboZero**。按 **Z 左、X 下、C 右、F 上**移动焦点；**连续长按 Esc 3 秒**退出并返回启动器。

把角色 BIN 放在**启动器用户**自己的 `~/amiibo_sim/` 中，并确保该用户有读取权限。如果此前通过 root 的 ADB shell 把数据放在 `/root/amiibo_sim/`，普通启动器用户不会扫描到。也可在手动运行设备版时通过 `--data-dir DIRECTORY` 指定目录。

每次打开时，NFC 模拟开关默认关闭。按 **Enter** 后先显示启动提示，连接并锁定 SPI 设备、识别芯片和完成硬件初始化；成功后才进入 **Role BINs**，失败则留在首页显示错误，可按 Enter 重试。用 **F/X** 逐行移动、**Z/C** 翻页。上次确认的 BIN 仍有效时会自动高亮，按 **Enter** 重新校验并开始该角色的 NFC 监听。监听中按 **Z** 返回选择页，保留已初始化的硬件连接。按 **N** 手动关闭并释放 NFC；长按 **Esc 3 秒**退出并关闭。数据目录中的 `.amiibozero-last-role` 只保存文件名，不保存 NFC 开关或角色数据。设备已有 Noto Sans CJK 时可显示中文文件名。

确认有效角色后，设备版默认进入 **Auto UID ON**：加载数据目录中的 160 字节组合密钥（优先 `key_retail.bin`，不存在时用 `retail.bin`），严格校验原始角色的两份 HMAC，生成随机 NXP UID，同步更新 BCC、派生密码与 PACK，并重新计算 HMAC、重新加密。重新解密校验通过后，才把新身份交给 NFC 芯片。两个密钥文件名可以是同一份组合密钥，不需要分别准备加密和解密文件。密钥缺失或角色 HMAC 错误会阻止启动，不会默默跳过校验。

**重复扫描**：选择 BIN → 在游戏中扫描 → 移开天线至少 1 秒 → 看到 `UID #` 递增 → 再扫描。同一射频会话内完整发送全部 130 页数据后，射频场连续消失 1 秒才自动更换身份；交互期间保持同一个 UID。只查询版本、签名或读取部分页不会触发轮换；中断后保留 UID 供重试，界面显示已发送页数。按 **R** 可以请求下一身份，仍需移开天线。计数表示身份已换新，不表示游戏已接受或奖励已领取；能否绕过每日限制取决于具体游戏并需要实测。

协议支持 `READ`、完整 135 页 `FAST_READ`（512 字节 FIFO 加续传）、`GET_VERSION`、`HALT`、密码认证、4-bit ACK/NAK、用户页 `WRITE` / `COMPATIBILITY_WRITE` 和 `READ_CNT`。读操作隐藏 PWD/PACK，写操作检查静态/动态锁和密码保护。写入只影响当前内存会话，不修改源 BIN；下一身份从原始角色生成。UID、锁页和配置页不接受写入。`READ_SIG` 返回明确记录的 32 字节全零模拟占位，因为 540 字节 BIN 不包含原厂 ECC 签名；组合密钥不能生成 NXP 原厂 ECC 签名。启动失败显示错误，不显示等待动画；退出不会切断 LCD 共享扩展供电。

设备需由 BSP 提供 `/dev/spidev0.2`（CS2 / GPIO22），且扩展板已供电。官方读卡 App 动态加载的 overlay 只维持到下次重启；AmiiboZero 不会自动执行特权硬件初始化。若重启后 NFC 启动失败，可在设备上以 root 执行本仓库的 `sh hardware/setup-cardputer.sh`，然后重启一次。脚本备份启动配置，在 `config.txt` 启用 BSP overlay，并安装开机供电服务，设置 GPIO 模式、GPIO26 POWER_EN 和 EXT5V；退出 App 时不关闭共享供电。备份路径会在执行后打印。此配置将扩展接口用于 Cap GPIO，不适用于同时使用该接口 USB 功能的场景。安装包也提供 `/usr/share/amiibozero/hardware/setup-cardputer.sh`。详见 [官方硬件说明](https://github.com/CardputerZero/Cap-CC1101-NFC#hardware)。不要让官方读卡 App 和本项目同时占用扩展板。

可选构建 `cap_listen_probe` 目标，在设备上运行 `./build-cardputer/cap_listen_probe`。它用合成标签监听 30 秒，记录外部 RF 场、选卡和 `READ` 发送完成，不读取角色或密钥。通过探针测试不等于已通过 Switch 验收。

在设备上的 ARM64 Linux 环境构建（CMake 3.28+、C/C++ 编译器、`libfreetype-dev`、`libssl-dev`、Debian 打包工具）：

```sh
cmake -S . -B build-cardputer -DAMIIBOZERO_CARDPUTER=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-cardputer --parallel 2
(cd build-cardputer && cpack)
sudo dpkg -i build-cardputer/amiibozero_0.3.1_arm64.deb
```

开发验证可显式构建 `amiibo_crypto_check`，在设备上运行 `./build-cardputer/amiibo_crypto_check /home/weijia/amiibo_sim`。它只读本地数据，输出角色校验数量和生成身份数量，不输出密钥、角色内容或新 UID。`cap_cycle_check` 需要真实 NFC 硬件和同一数据目录，用于验证手动请求后实际切换到第 2、3 个身份；运行前退出其他 NFC App。默认 CTest 只使用公开合成向量。
