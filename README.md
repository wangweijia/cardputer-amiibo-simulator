# AmiiboZero

面向 M5Stack Cardputer Zero 的 amiibo 模拟器项目，详见[技术设计](./TECHNICAL_DESIGN.md)。设备版已接入 ST25R3916 被动 NFC-A 监听，尚未通过 Switch amiibo 识别验收。

## 本地构建与测试

需要 CMake 3.16 或更新版本，以及支持 C++17 的编译器。不需要连接设备，也不需要角色数据或密钥。

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

每次打开时，NFC 模拟开关默认关闭。按 **Enter** 开启并进入 **Role BINs**，用 **F/X** 逐行移动、**Z/C** 翻页。上次确认的 BIN 仍有效时会自动高亮，但必须再按 **Enter** 才会重新校验并请求启动。按 **N** 手动关闭功能；长按 **Esc 3 秒**退出并关闭。数据目录中的 `.amiibozero-last-role` 只保存文件名，不保存 NFC 开关或角色数据。设备已有 Noto Sans CJK 时可显示中文文件名。

确认有效角色后，设备版使用该 BIN 的 UID 和数据启动被动监听，并显示旋转动画提示正在等待读卡器。目前支持 `READ`、`GET_VERSION` 和最多 15 页的 `FAST_READ`；不处理写入、认证或签名请求，不保证 Switch 兼容。启动失败会显示错误，不显示等待动画。退出监听只停止 NFC 芯片，不切断 LCD 共享的扩展供电。

设备需由 BSP 提供 `/dev/spidev0.2`（CS2 / GPIO22），且扩展板已供电。若设备重启后缺少该节点，按 [官方 Cap NFC 应用的硬件说明](https://github.com/CardputerZero/Cap-CC1101-NFC#hardware) 初始化扩展板。不要让官方读卡 App 和本项目同时占用扩展板。

可选构建 `cap_listen_probe` 目标，在设备上运行 `./build-cardputer/cap_listen_probe`。它用合成标签监听 30 秒，记录外部 RF 场、选卡和 `READ` 发送完成，不读取角色或密钥。通过探针测试不等于已通过 Switch 验收。

在设备上的 ARM64 Linux 环境构建（CMake 3.28+、C/C++ 编译器、`libfreetype-dev`、Debian 打包工具）：

```sh
cmake -S . -B build-cardputer -DAMIIBOZERO_CARDPUTER=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-cardputer --parallel 2
(cd build-cardputer && cpack)
sudo dpkg -i build-cardputer/amiibozero_0.2.3_arm64.deb
```
