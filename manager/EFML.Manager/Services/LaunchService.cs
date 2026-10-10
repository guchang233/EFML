using System.ComponentModel;
using System.Diagnostics;

namespace EFML.Manager.Services;

internal static class LaunchService
{
    public static bool GameRunning() => Process.GetProcessesByName("Endfield").Length > 0;

    // Starts the game with EFML. Returns a message for the user.
    public static async Task<string> LaunchAsync(ManagerSettings settings)
    {
        if (!GameDiscovery.IsGameExecutable(settings.GamePath)) throw new InvalidOperationException("请先在设置中选择 Endfield.exe");
        if (GameRunning()) throw new InvalidOperationException("游戏已在运行。EFML 需要在游戏启动时加载，请先关闭游戏。");
        await RuntimeService.InstallAsync();
        string gameDirectory = Path.GetDirectoryName(settings.GamePath)!;
        ProxyStatus proxy = await ProxyService.InspectAsync(settings.GamePath);

        if (settings.LoadMode == LoadMode.Proxy || proxy.State == ProxyState.Installed)
        {
            if (proxy.State != ProxyState.Installed) await ProxyService.InstallAsync(settings.GamePath);
            Process.Start(new ProcessStartInfo(settings.GamePath, settings.LaunchArguments)
            {
                UseShellExecute = true,
                WorkingDirectory = gameDirectory
            });
            return "游戏已启动，EFML 代理会在游戏中加载运行时。";
        }

        if (proxy.State == ProxyState.Conflict)
            throw new InvalidOperationException("游戏目录里有其他加载器的 xinput1_4.dll，它会和 EFML 同时加载。请先移除它再用注入方式启动。");
        if (proxy.State == ProxyState.UpdateAvailable) await ProxyService.UninstallAsync(settings.GamePath);

        string arguments = $"--game \"{settings.GamePath}\" --data-root \"{AppPaths.DataRoot}\"";
        if (settings.LaunchArguments.Trim().Length > 0) arguments += " -- " + settings.LaunchArguments.Trim();
        Process injector;
        try
        {
            injector = Process.Start(new ProcessStartInfo(Path.Combine(AppPaths.InstalledRuntime, "EFML.Injector.exe"), arguments)
            {
                UseShellExecute = true,
                Verb = "runas",   // Like Better-Endfield's injector: needs administrator rights.
                WindowStyle = ProcessWindowStyle.Hidden
            }) ?? throw new InvalidOperationException("注入器没有启动");
        }
        catch (Win32Exception exception) when (exception.NativeErrorCode == 1223)
        {
            throw new InvalidOperationException("已取消管理员权限确认，游戏未启动。");
        }
        using var timeout = new CancellationTokenSource(TimeSpan.FromMinutes(2));
        await injector.WaitForExitAsync(timeout.Token);
        return injector.ExitCode switch
        {
            0 => "游戏已启动，EFML 已注入。",
            2 => throw new InvalidOperationException("注入器参数错误"),
            3 => throw new InvalidOperationException("运行时文件缺失，请在设置中重新安装运行时"),
            4 => throw new InvalidOperationException("游戏没有启动"),
            5 => throw new InvalidOperationException("注入失败，详见 logs\\loader.log"),
            6 => throw new InvalidOperationException("游戏目录里有其他加载器的 xinput1_4.dll"),
            int code => throw new InvalidOperationException($"注入器失败（{code}）")
        };
    }
}
