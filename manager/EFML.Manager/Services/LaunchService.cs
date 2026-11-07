using System.ComponentModel;
using System.Diagnostics;
using System.Text;

namespace EFML.Manager.Services;

// EFML loads inside the game through the xinput proxy. Two ways to start the game:
//   direct  : EFML.Launcher.exe borrows the installed official launcher as the game's parent, so the
//             client's launcher check passes without showing the launcher UI (one UAC prompt).
//   guided  : only prepare the proxy and tell the user to click Start Game in the official launcher.
internal static class LaunchService
{
    public sealed record LaunchResult(bool Started, string Message);

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

    // Starts the game through EFML.Launcher.exe. The helper needs administrator rights because both
    // Endfield.exe and the official launcher are elevated; Windows shows the normal UAC prompt.
    public static async Task<LaunchResult> LaunchDirectAsync(ManagerSettings settings)
    {
        await PrepareAsync(settings);
        string helper = Path.Combine(AppPaths.InstalledRuntime, "EFML.Launcher.exe");
        if (!File.Exists(helper))
            throw new FileNotFoundException("运行时缺少 EFML.Launcher.exe，请在「设置」里重新安装运行时。", helper);

        string arguments = "--hide-console --game " + Quote(settings.GamePath);
        if (!string.IsNullOrWhiteSpace(settings.OfficialLauncherPath))
            arguments += " --parent " + Quote(settings.OfficialLauncherPath.Trim().Trim('"'));

        // Resolve and verify the official launcher before asking for elevation, so a wrong path or an
        // unsigned file reports here instead of behind a UAC prompt.
        await PreflightAsync(helper, arguments + " --check");

        var start = new ProcessStartInfo
        {
            FileName = helper,
            Arguments = arguments,
            WorkingDirectory = AppPaths.InstalledRuntime,
            UseShellExecute = true,
            Verb = "runas"
        };
        try
        {
            Process.Start(start);
        }
        catch (Win32Exception exception) when (exception.NativeErrorCode == 1223)
        {
            return new LaunchResult(false, "已取消管理员授权（UAC），游戏没有启动。");
        }
        return new LaunchResult(true,
            "已请求管理员权限，游戏正在启动。官方启动器界面不会出现；进入游戏后这里会显示宿主和模组状态。");
    }

    // Runs EFML.Launcher.exe --check unelevated (its manifest is asInvoker) and reports the first
    // line of its diagnostics when discovery or the signature check fails.
    private static async Task PreflightAsync(string helper, string arguments)
    {
        var start = new ProcessStartInfo
        {
            FileName = helper,
            Arguments = arguments,
            WorkingDirectory = AppPaths.InstalledRuntime,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8,
            StandardErrorEncoding = Encoding.UTF8
        };
        using Process process = Process.Start(start) ??
            throw new InvalidOperationException("无法启动 EFML.Launcher.exe。");
        Task<string> output = process.StandardOutput.ReadToEndAsync();
        Task<string> error = process.StandardError.ReadToEndAsync();
        await process.WaitForExitAsync();
        if (process.ExitCode == 0) return;
        string details = (await error).Trim();
        if (details.Length == 0) details = (await output).Trim();
        throw new InvalidOperationException(
            "官方启动器未就绪，游戏没有启动。\n" +
            (details.Length > 0 ? details : "EFML.Launcher.exe 退出码 " + process.ExitCode));
    }

    private static string Quote(string value) => "\"" + value.Replace("\"", "\\\"") + "\"";
}
