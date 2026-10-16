# EFML 模组开发指南

EFML 模组是一个 ZIP 包：根目录一个 `module.json`，加上一个 Windows x64 DLL 和/或一个网页。包格式、原生接口和网页桥都与 Better-Endfield 第三方模块（包格式 1 / ABI 1）兼容：为 Better-Endfield 3.x 或 Better Endfield Next 4.x 编译的模块无需重新编译即可在 EFML 中加载。

公开头文件在 `native/include/EFML/`（发布包里是 `sdk/include/EFML/`）：

| 头文件 | 内容 |
|---|---|
| `Module.h` | 模块入口、宿主接口、EFML 扩展；新模块只需包含它 |
| `ModuleApi.h` | IL2CPP 辅助函数（`BE_HostApiV1`）与结果码 |
| `HookChain.h` | 共享 Hook 链 |

完整示例见 `sdk/examples/echo/`。

## 包结构

```
module.json
native/windows-x64/my.module.dll
ui/index.html            （可选）
ui/app.js  ui/style.css
```

```json
{
  "format": 1,
  "abi": 1,
  "id": "author.my-module",
  "name": "我的模组",
  "author": "作者",
  "version": "1.0.0",
  "description": "一句话说明",
  "libraries": { "windows-x64": "native/windows-x64/my.module.dll" },
  "ui": "ui/index.html",
  "default_configuration": { "enabled_feature": true },
  "dependencies": [],
  "load": "runtime"
}
```

- `id`：`[A-Za-z0-9][A-Za-z0-9_.-]{0,95}`。`efml.`、`betterendfield.`、`betterendfieldnext.` 开头的名字是保留的。
- 包内路径一律使用 `/`，不能出现 `..`、盘符或绝对路径。
- `dependencies`：依赖的其他模组 id。被依赖的模组先启动；它停用时，依赖它的模组也会停下。
- `load`（EFML 新增，可选）：默认 `"runtime"`，等游戏的 IL2CPP 运行时就绪后再加载。只有需要在 Unity 启动前就 Hook 的模组才写 `"early"`。

## 入口与生命周期

```cpp
#include "EFML/Module.h"

static const BE_ThirdPartyHostV1* host;
static const EFML_HostExtV1* efml;   // 在 Better-Endfield 下运行时为空

static BE_Result BE_CALL Initialize(const BE_ThirdPartyHostV1* provided, const char* configuration_json) {
    host = provided;
    efml = EFML_GetExtension(provided);
    host->log(host->context, "started");
    return BE_Result_Ok;
}
static BE_Result BE_CALL Configure(const char* configuration_json) { return BE_Result_Ok; }
static BE_Result BE_CALL Message(const char* request_id, const char* body_json) {
    return host->reply(host->context, request_id, BE_Result_Ok, "{\"ok\":true}");
}
static void BE_CALL Shutdown() { host = nullptr; }

static const BE_ThirdPartyModuleV1 my_module{sizeof(BE_ThirdPartyModuleV1), 1, "author.my-module",
    Initialize, Configure, Message, Shutdown};

EFML_DEFINE_MODULE(my_module)   // 导出 EFML_GetModuleV1，以及两个 Better-Endfield 入口名
```

- `initialize` 与 `on_message` 必须提供，`configuration_changed` 和 `shutdown` 可以为空。
- 在模组页面或管理器中停用模组时，EFML 会调用 `shutdown`；之后重新启用会再次调用 `initialize`。DLL 一旦加载就常驻到游戏退出，因此安装新版本需要重启游戏。
- `initialize` 返回 `BE_Result_NotReady` 时，EFML 每 2 秒重试一次，最多 30 次。
- 所有回调都在同一个模块工作线程上**依次**执行。EFML 已把这个线程挂接到 IL2CPP，可以直接调用 `runtime_invoke` 等函数。Better-Endfield 的工作线程没有挂接，需要兼容它的模组仍应自行处理。
- 字符串只在回调期间有效，需要保留的请自行复制。

## IL2CPP 与 Hook

`host->get_runtime(host->context)` 返回 `BE_HostApiV1`，运行时未就绪时返回空。它按名称解析方法、字段和类，并提供 `runtime_invoke`、`object_new`、`string_new`、`gchandle_*` 等函数：

```cpp
const BE_HostApiV1* api = host->get_runtime(host->context);
BE_MethodDescriptorV1 d{"Gameplay.Beyond.dll", "Beyond.Gameplay.Core", "PlayerController",
                        "GetMainCharacter", nullptr, "Beyond.Gameplay.Core.Entity", 0};
BE_ResolvedMethodV1 m{};
api->resolve_method(api->context, &d, &m);
```

`host->hooks` 是共享 Hook 链。多个模组 Hook 同一个函数时按注册顺序串联，`next` 指向链上的下一个节点（最后是原函数）。模组停用时，它的节点会自动变成直通。

```cpp
using Fn = void(*)(void* self, float dt, const void* method);
static Fn next;
static void Detour(void* self, float dt, const void* method) { next(self, dt, method); /* ... */ }
uint64_t handle;
host->hooks->create(host->hooks->context, host->module_id, m.method_pointer,
                    (void*)&Detour, (void**)&next, &handle);
```

## EFML 扩展

`EFML_GetExtension(host)` 返回 `EFML_HostExtV1`；在 Better-Endfield 下运行时返回空，请做判断。

### 游戏线程

```cpp
static void BE_CALL OnFrame(void* user, uint64_t frame) { /* 每帧在 Unity 主线程执行 */ }
static void BE_CALL Once(void* user) { /* 下一帧在主线程执行一次 */ }

uint64_t frame_handle;
efml->add_frame_callback(host->context, &OnFrame, nullptr, &frame_handle);
efml->post_to_game_thread(host->context, &Once, nullptr);
efml->remove_frame_callback(host->context, frame_handle);
```

- EFML 把游戏每帧都会调用的方法（场景中的 `CameraManager.TailLateTick`、UGUI 的 `EventSystem.Update`）作为帧来源，并用 `Time.frameCount` 去重，保证每帧只执行一次。
- 游戏线程就绪之前提交的任务会排队等候；就绪时会发出 `game_thread_ready` 事件。
- 模组停用时，EFML 会先移除它的帧回调和未执行的任务，并等当前这一帧执行完，再调用 `shutdown`。
- 需要先恢复游戏原生状态的 EFML 模组可返回 `EFML_ModuleWithShutdownBarrierV1`（保留 V1 结构为首字段），并在 `prepare_shutdown` 中关闭新操作、请求游戏线程恢复。返回 `BE_Result_NotReady` 时普通停用延后，回调与钩子仍保留；EFML 随后重试。只有返回 `BE_Result_Ok` 才移除回调并调用 `shutdown`。使用前通过 `EFML_HasShutdownBarrier(efml)` 检查宿主支持；其他宿主和旧版 EFML 不提供保证。进程退出时会尝试一次，然后继续结束进程，不能用该机制代替游戏运行期间的恢复确认。
- `is_game_thread()` 和 `frame_count()` 可以用来判断当前线程和帧计数。

### 服务（模组之间共享接口）

```cpp
struct MyApiV1 { int (*answer)(); };
static const MyApiV1 api{[] { return 42; }};
efml->register_service(host->context, "author.my-module.api", 1, &api);      // 提供方，通常在 initialize 里

auto* other = (const MyApiV1*)efml->get_service(host->context, "author.my-module.api", 1);   // 使用方
```

使用方要在 `dependencies` 中列出提供方，确保提供方先启动。提供方停用时，它注册的服务会被撤回（并发出 `service_withdrawn` 事件）。

### 宿主事件

```cpp
static void BE_CALL OnEvent(void* user, const char* json) { /* {"type": "..."} */ }
efml->subscribe(host->context, &OnEvent, nullptr, nullptr);
```

| type | 时机 |
|---|---|
| `runtime_ready` | IL2CPP 运行时就绪 |
| `game_thread_ready` | 游戏线程找到，`source` 为帧来源 |
| `module_ready` / `module_stopped` | 某个模组启动 / 停止，`id` |
| `service_registered` / `service_withdrawn` | 服务注册 / 撤回，`name`、`provider` |

事件在模块工作线程上投递。

## 模组页面

模组可以自带一个网页（`ui`），在管理器里点击「打开模组页面」后，显示在独立窗口中（WebView2）。每个已安装的版本都有独立的 https 来源，页面只能读取自己包内的文件，不能跳转到外部网址。请把依赖的 JS、CSS 和字体都打进包里，不要引用 CDN。

```js
const host = window.efml;               // 同一个对象也叫 window.betterEndfield / window.betterEndfieldNext
const config = await host.readConfig();
await host.saveConfig({ ...config, enabled_feature: false });   // 游戏没运行时也能保存
const unsubscribe = host.onmessage(message => {
  if (message.kind === 'reply') console.log(message.request_id, message.result, message.body);
  if (message.kind === 'event') console.log(message.body);
});
await host.send({ action: 'count' });   // 交给原生模组的 on_message
const status = await host.status();    // { connected, module }
```

请求 15 秒超时。消息和事件不会持久化，队列有容量上限。

## 本地 RPC

游戏中的宿主在 `127.0.0.1:<port>` 上提供 HTTP 接口，端口和令牌写在 `%LOCALAPPDATA%\EFML\modules\index.json` 里，请求需带 `Authorization: Bearer <token>`：

| 请求 | 内容 |
|---|---|
| `GET /status` | 宿主信息（版本、运行时、游戏线程、服务）与所有模组状态、最近日志 |
| `POST /send` | `{module_id, request_id, body}` → 交给模组 `on_message` |
| `POST /poll` | `{module_id}` → 取出该模组的回复和事件 |
| `POST /configure` | `{module_id, configuration}` → 调用 `configuration_changed` |

## 开发流程

1. 编译 DLL，按上面的结构准备一个文件夹。
2. 注册开发文件夹，EFML 直接从该文件夹加载、不复制：在管理器里点「开发：注册文件夹」，或执行 `efml add --dev <文件夹>`。
3. 在 EFML 里点「启动游戏」准备加载组件，再用鹰角官方启动器开始游戏（EFML 不直接启动游戏）。查看日志：管理器「启动」页，`efml logs <id>`，或 `%LOCALAPPDATA%\EFML\logs\EFML.log`。
4. 不开页面也能给模组发消息：`efml send <id> "{\"action\":\"ping\"}"`。
5. 已加载的 DLL 不会被替换：改动后用新的模块身份注册开发文件夹（`efml add --dev`），游戏正在运行时也会立即尝试加载；沿用同一个身份则需要重启游戏。
6. 发布时把文件夹内容打成 ZIP（`module.json` 必须在 ZIP 根目录）。

本地编译示例：

```bat
cl /nologo /std:c++20 /EHsc /O2 /MT /utf-8 /LD /I<sdk>\include my_module.cpp /Fe:my.module.dll
```

建议使用 `/MT`（静态 CRT），避免与游戏自带的运行库冲突。
