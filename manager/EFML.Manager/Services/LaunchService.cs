using System.Diagnostics;

namespace EFML.Manager.Services;

// EFML does not start the game. Endfield 1.6.3 only accepts a start from the official launcher,
// so this service just makes sure the runtime and the xinput proxy are in place; the user then
// starts the game from the official launcher and the proxy loads EFML.
internal static class LaunchService
{
    public static bool GameRunning() => Process.GetProcessesByName("Endfield").Length > 0;

    // Installs the runtime and the proxy if needed and returns the resulting proxy state.
    public static async Task<ProxyStatus> PrepareAsync(ManagerSettings settings)
    {
        if (!GameDiscovery.IsGameExecutable(settings.GamePath)) throw new InvalidOperationException("请先在设置中选择 Endfield.exe");
        await RuntimeService.InstallAsync();
        ProxyStatus proxy = await ProxyService.InspectAsync(settings.GamePath);
        if (proxy.State == ProxyState.Conflict)
            throw new InvalidOperationException("游戏目录里有其他加载器的 xinput1_4.dll，它会和 EFML 同时加载。请先用它自带的工具卸载，再回来。");
        if (proxy.State != ProxyState.Installed)
        {
            if (GameRunning()) throw new InvalidOperationException("游戏正在运行，无法安装代理。请先关闭游戏。");
            await ProxyService.InstallAsync(settings.GamePath);
            proxy = await ProxyService.InspectAsync(settings.GamePath);
        }
        return proxy;
    }
}
