# EFML — Endfield Mod Loader

《明日方舟：终末地》（Windows x64）的模组加载器与管理器。EFML 以 [Better-Endfield](https://github.com/Dr-hydra/Better-Endfield) 的核心组件为基础：游戏内加载器、IL2CPP 解析、共享 Hook 链和第三方模块宿主。它只负责加载模组，不包含内置的功能模块。

- **兼容 Better-Endfield 第三方模块**：包格式（`module.json` format 1 / ABI 1）、模块列表、本地 RPC 和模块页面接口都与 Better-Endfield 一致。为 Better Endfield 3.x（`BetterEndfield_GetThirdPartyModuleV1`）或 Next 4.x（`BetterEndfieldNext_GetThirdPartyModuleV1`）编译的模块无需重新编译即可加载。
- **面向模组作者的扩展**：游戏线程任务与每帧回调、已挂接 IL2CPP 的模块工作线程、模组之间的服务、宿主事件、运行时就绪后加载及自动重试。
- **不直接启动游戏**：终末地 1.6.3 只接受从鹰角官方启动器启动。EFML 在游戏目录放一个 `xinput1_4.dll` 代理，你用官方启动器开始游戏时，游戏会自动加载它，再由它加载已启用的模组。EFML 的「启动游戏」按钮只负责准备加载组件并告诉你怎么启动。

## 下载与使用

从 [Releases](https://github.com/guchang233/EFML/releases/latest) 下载：

| 文件 | 说明 |
|---|---|
| `EFML-<版本>-Setup.exe` | 安装包，装到 `%LOCALAPPDATA%\Programs\EFML`，不需要管理员权限 |
| `EFML-<版本>-portable.zip` | 便携版，解压后运行 `EFML.exe` |
| `EFML-Echo-example.zip` | 示例模组，可在「模组」页直接导入 |

需要 Windows 10/11 x64 与 WebView2 运行时（Windows 11 自带）。

1. 首次运行时，EFML 会查找游戏并安装运行时。
2. 在「模组」页导入模组的 `.zip`，并启用它。
3. 在「启动」页点击「启动游戏」：EFML 会安装（或更新）加载组件，并显示启动步骤。然后打开鹰角启动器，点击「开始游戏」。页面上会实时显示游戏、宿主、游戏线程和各模组的状态，以及日志。加载组件只需安装一次，以后直接从官方启动器开始游戏同样生效。

EFML 不读取登录凭据，也不构造游戏启动参数；账号登录和游戏参数都由官方启动器处理。

模组、日志和设置保存在 `%LOCALAPPDATA%\EFML`；设置环境变量 `EFML_HOME` 可以改用其他目录。卸载 EFML 时会保留这个目录，并移除 EFML 放入游戏目录的代理文件。

也可以用命令行工具 `runtime\efml.exe` 完成同样的操作，例如 `efml status`、`efml add <模组.zip>`、`efml launch`、`efml logs`。运行 `efml help` 查看全部命令。

```powershell
# 安装加载组件（代理）并显示启动步骤；不会启动游戏
runtime\efml.exe launch
# 游戏中也可导入并启用一个新模块身份，无需重新启动官方启动器
runtime\efml.exe add --dev "D:\MyMod\package"
runtime\efml.exe enable my.mod
runtime\efml.exe status
```

热加载取决于模组自己的初始化、依赖和停用清理能力。修改已经加载的 DLL 时，使用新模块身份或正常重启游戏；不能保证所有模组都能原地替换。

## 与 Better-Endfield 的关系

EFML 的加载器、宿主和模块接口提取自 Better-Endfield（基于 Better Endfield Next 4.0.0），按 AGPL-3.0 使用，并在派生的源文件中注明出处。两者的定位不同：

- **Better-Endfield** 是功能完整的增强工具，内置相机、自定义模型、语音、音乐、战斗数据等功能，并提供 Android 版本和模组下载站。
- **EFML** 只做模组加载这一件事，作为其他模组（例如 EndCraft、EF Link）共同依赖的基础。

Better Endfield Next 4.0.0 暂时隐藏了第三方模块的管理入口，并更换了模块入口名（见其[更新日志](https://github.com/Dr-hydra/Better-Endfield/blob/main/CHANGELOG.md)）。EFML 同时识别新旧入口名，因此为任一版本编写的第三方模块都可以继续使用。

### EFML 额外提供的内容

**模组接口扩展**

以下扩展通过在宿主接口末尾追加字段实现，不影响已有模块：

- **游戏线程**：`post_to_game_thread` 在下一帧于 Unity 主线程执行任务，`add_frame_callback` 每帧执行。模组不必再各自 Hook 每帧调用的游戏方法。
- **工作线程已挂接 IL2CPP**：模块回调中可以直接调用 IL2CPP 辅助函数。
- **服务**：模组可以注册具名、带版本的接口，其他模组按名称获取；提供方停用时自动撤回。
- **宿主事件**：运行时就绪、游戏线程就绪、模组启动/停止、服务注册/撤回。
- **加载时机与重试**：默认在游戏运行时就绪后加载；`initialize` 返回「未就绪」时自动重试。需要提前加载的模组可以在 `module.json` 中声明 `"load": "early"`。
- **模块列表校验**：模块列表无效时整体拒绝，不会部分生效。

**开发与排错**

- 注册开发文件夹，在原位置加载模组，无需打包、复制。
- 命令行工具 `efml.exe`，可以安装、启用模组，向模组发送消息，查看日志，适合脚本和 CI。
- 管理器实时显示宿主、游戏线程和各模组的状态与日志。
- 宿主代码按可读性重写，并附带不依赖游戏的自动化测试。

**运行方式**

- 只加载用户安装的模组，没有内置功能模块。
- `xinput1_4.dll` 代理带有自身标识；EFML 不会覆盖或删除其他加载器放在游戏目录中的同名文件。

### 当前限制

- 1.0.0 已通过自动化测试和 CI 构建，**尚未经过完整的游戏内验证**。
- EFML 本身不提供游戏功能；需要安装模组才有效果。
- 不能与 Better-Endfield 在同一游戏进程中同时使用：两者会各自 Hook 游戏。同一时间请只使用其中一个。
- 仅支持 Windows x64，没有 Android 版本。
- 模组接口沿用 Better-Endfield 第三方 ABI v1。如果 Better-Endfield 将来推出新版接口，EFML 会评估是否跟进。
- 与所有注入类工具一样，游戏更新可能导致帧来源或方法解析失效；使用模组可能违反游戏服务条款并导致账号受限。EFML 不会关闭或绕过反作弊。

## 开发模组

见 [docs/MODULE-API.md](docs/MODULE-API.md)，示例模组在 [sdk/examples/echo](sdk/examples/echo)。发布包的 `sdk/` 目录中附带头文件和示例。

## 目录结构

| 路径 | 内容 |
|---|---|
| `native/include/EFML/` | 公开 SDK 头文件（`Module.h`、`ModuleApi.h`、`HookChain.h`） |
| `native/include/BetterEndfield*/` | 兼容 Better-Endfield 源码的头文件 |
| `native/host/` | `EFML.Host.dll`：模块宿主、RPC、Hook 管理、IL2CPP 解析、游戏线程 |
| `native/loader/` | `xinput1_4.dll` 代理（`EFML.Bootstrap.dll`、`EFML.Injector.exe` 是旧版注入组件，管理器和命令行已不再使用） |
| `native/cli/` | 命令行工具 `efml.exe` |
| `native/tests/` | 宿主测试（不需要游戏） |
| `manager/EFML.Manager/` | 管理器（C# / WinUI 3） |
| `installer/` | Inno Setup 安装脚本 |
| `sdk/examples/echo/` | 示例模组 |

## 构建

每次推送都由 GitHub Actions 构建并测试（`.github/workflows/build.yml`），推送 `v*` 标签时自动发布。

本地编译原生部分有两种方式。一是只用 MSVC，不需要 CMake：

```bat
native\build.cmd
```

二是使用 CMake：

```bat
cmake -S native -B build\native-cmake -A x64
cmake --build build\native-cmake --config Release
ctest --test-dir build\native-cmake -C Release
```

管理器需要 .NET 9 SDK：

```bat
dotnet publish manager\EFML.Manager\EFML.Manager.csproj -c Release -r win-x64 --self-contained true -p:Platform=x64 -p:PublishSingleFile=true
```

## 许可

AGPL-3.0，与 Better-Endfield 相同。见 [LICENSE](LICENSE) 与 [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)。本项目与鹰角网络及 Better-Endfield 作者无关。
