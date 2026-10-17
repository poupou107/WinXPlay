# WinXPlay 项目长期备忘

## 本机编译环境（2026-10-17 搭建）
- 文档工具链是 VS2008 + Windows SDK 6.0A + clang-cl，本机没有；实际使用 VS2019 Community（MSVC 14.29.30133，位于 D:\app\Microsoft Visual Studio\2019\Community）+ Windows 10 SDK 10.0.19041.0（C:\Program Files (x86)\Windows Kits\10）。
- 垫片位于 `.local/toolchains/`（均 gitignore）：`msvc/bin/amd64`、`msvc/include`、`msvc/lib/amd64` 为目录联接（include 是 MSVC+UCRT 合并拷贝，不是联接）；`winsdk/Include`、`winsdk/Lib/x64` 为 ucrt+um(+shared) 合并拷贝，用于复现 VS2008 的 INCLUDE/LIB 两段式布局。
- `.local/settings.ps1`：CARPLAY_CLANG 指向 MSVC cl.exe（可吃 clang-cl 那套 flag）；OpenSSL DLL 用 GIMP 自带的 `D:\app\GIMP 3\bin\lib(crypto|ssl)-3-x64.dll`；MRN_PYTHON 用 `E:\Python\python.exe`（3.11.2，pefile/cryptography/unicorn/Pillow 版本与 requirements-dev.txt 完全一致；沙箱代理装不了 unicorn，别用受管 venv）。
- 编译入口：`./build.ps1 -Target Windows`，产物 `build/carplay/windows-x64/WinXPlay.exe`。已知无害警告：usb.cpp WSAAddressToStringA 弃用。
- 未配置 CARPLAY_IDENTITY，运行时身份需自行准备 certificate.p7b + identity.pk8。

## iPhone USB 控制通道（2026-10-17 修复）
- 本机 Apple Mobile Device Support 14.5.0.7 把 iPhone mux 接口（MI_01）绑定为 WinUSB（INF 注册 {664be590}，注释 "New iTunes WinUSB interface"），不再用 USBAAPL64.sys；旧接口 {f0b32be3} + 私有 IOCTL 0x2200A0 只适用于旧苹果驱动栈。
- `carplay/platform/windows/usb.cpp` 已改为优先 WinUSB 通道（WinUsb_ControlTransfer），旧路径保留回退；`#pragma comment(lib,"winusb.lib")` 自包含链接。
- 诊断工具：`E:\Python\python.exe carplay\tests\windows\usb_configuration.py`（--legacy 测旧路径，--enable-carplay 测 0x52 模式切换）。
