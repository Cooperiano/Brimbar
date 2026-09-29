# Contributing

欢迎提交问题和补丁。这个项目会接触 Windows Shell 与托盘交互，因此优先保证可退出、可回滚和 Explorer 不受损。

## 开发要求

- Windows 11 x64
- Visual Studio 2022（Desktop development with C++）
- CMake 3.24 或更高版本

## 提交前检查

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

请保持未知 Windows build 默认拒绝加载，不要在缺少版本探测、崩溃熔断和自动回滚时启用 Explorer 注入。
提交中不得包含凭据、用户名、本机绝对路径、Cookie、完整崩溃转储或其他个人数据。
