# 启动终末地

终末地 1.6.3 在启动时由 `OneChannelLC.dll` 执行一次启动校验（日志里是 `Start launch check` / `Parent process check success`）：

1. 取游戏的**直接父进程**（不遍历祖先）；
2. 用 `QueryFullProcessImageNameW` 读父进程路径，文件名与允许列表（`Games.exe`）逐项比较；
3. 对该磁盘文件执行 `WinVerifyTrust`；
4. 比对该文件签名证书的指纹。

任一步不通过，游戏提示「请通过官方启动器打开游戏」并退出（内部码 10100 / 10101 / 10102）。

EFML 不修改这条校验，也不伪造签名。它用下面两种方式之一让游戏通过：

## 方式一：直接启动（默认按钮）

`EFML.Launcher.exe` 用本机**正版、签名有效**的官方启动器文件做父进程：

1. 从注册协议（`hypergryphlauncher*` / `gryphlinklauncher*`）、卸载项和常见安装目录找到 `Games.exe`（版本目录取最新），校验 PE 头、Authenticode 签名和签名者（鹰角 / GRYPHLINE）；
2. 以 `CREATE_SUSPENDED | CREATE_NO_WINDOW` 创建隐藏的 `Games.exe`，**主线程永不恢复**——启动器界面不出现，它自己的单实例互斥锁也不会创建；
3. 用 `PROC_THREAD_ATTRIBUTE_PARENT_PROCESS` 指定该暂停进程为父进程，创建 `Endfield.exe`；
4. 恢复游戏线程；游戏退出后只清理自己创建的那个暂停父进程。

游戏仍然通过游戏目录里的 `xinput1_4.dll` 代理加载 EFML 宿主和已启用的模组。

需要一次管理员授权（UAC）：`Endfield.exe` 与官方启动器本身都要求管理员权限。

## 方式二：官方启动器

点「用官方启动器启动」只会准备/更新加载组件，然后你在鹰角启动器里点「开始游戏」。加载组件只需安装一次。

## 命令行

```powershell
# 直接启动：安装代理（如需要）→ 预检官方启动器 → 请求 UAC → 启动游戏
runtime\efml.exe launch --direct

# 只做预检：打印会使用的 game / home / parent / publisher，不启动、不提权
runtime\efml.exe launch --check

# 手动指定官方启动器（默认自动查找），或指定另一套 EFML_HOME（双开）
runtime\efml.exe launch --direct --parent "E:\Program Files\Hypergryph Launcher\1.6.0\Games.exe"
runtime\efml.exe launch --direct --home "D:\duo\client2\EFML"
```

没有安装代理、也不想往游戏目录写文件时，可以用注入通道（高级，命令行）：

```powershell
runtime\EFML.Injector.exe --game "D:\Arknights Endfield\Endfield.exe"
# 使用同一父进程桥；--manual-map 在恢复前手动映射 Host，--manual-map-late 等 GameAssembly.dll 就绪后再映射
```

`EFML.Launcher.exe --no-parent` 会直接创建游戏、不借用父进程，只适用于没有这条校验的客户端或诊断；国服会被拒绝。

## 双开

每个游戏实例用自己的 `EFML_HOME`。直启时把 `--home` 传给 `EFML.Launcher.exe`，它会为子进程设置 `EFML_HOME`，代理随后读取那一套 `loader.ini` 和模组列表。官方启动器不需要参与，也不需要第二份启动器副本。

## 限制与排查

- 本机必须保留正版鹰角启动器。启动器升级、证书或安装目录变化导致找不到可用文件时，EFML 会明确报错，不会换用其他程序。
- 直启依赖 Windows 的父进程属性；游戏本身仍可能因更新改变校验方式。失败时先看 `%LOCALAPPDATA%\EFML\logs\loader.log` 里的 `launcher:` 行和游戏自己的 `sdklogs`。
- 父进程进程会继承该暂停进程的 job/缓解策略；Better-Endfield 与本机实测可用，但这是系统行为层面的差异点。
- 直启与另一个加载器的 `xinput1_4.dll` 共存时，游戏能过校验，但 EFML 不会加载；管理器会在启动前提示冲突。
- 使用模组与直启可能违反游戏服务条款并导致账号受限；反作弊的行为不受 EFML 控制。EFML 不关闭、不修改反作弊。
