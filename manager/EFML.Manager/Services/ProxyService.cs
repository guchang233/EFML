using System.Text;
using System.Text.Json;

namespace EFML.Manager.Services;

internal enum ProxyState
{
    Unavailable,
    NotInstalled,
    Installed,
    UpdateAvailable,
    Conflict
}

internal sealed record ProxyStatus(ProxyState State, string Message, bool CanInstall, bool CanUninstall);

// The xinput1_4.dll proxy in the game folder. Derived from Better-Endfield's XInputDeploymentService
// (AGPL-3.0): copy through a verified temporary file, record ownership, and never overwrite or
// delete an xinput1_4.dll that another loader put there.
internal static class ProxyService
{
    public const string ProxyFileName = "xinput1_4.dll";
    private const string ManifestFileName = "EFML.xinput.install.json";
    private const string ProductId = "EFML.XInputProxy";
    private static readonly byte[] Marker = Encoding.ASCII.GetBytes("EFML-XInput-Proxy");

    private sealed record Ownership(string Product, string ProxyFile, string Sha256, DateTimeOffset InstalledUtc);

    private static string Source => Path.Combine(AppPaths.InstalledRuntime, ProxyFileName);

    public static bool IsEfmlProxy(string path)
    {
        try
        {
            byte[] data = File.ReadAllBytes(path);
            return data.AsSpan().IndexOf(Marker) >= 0;
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
        {
            return false;
        }
    }

    public static async Task<ProxyStatus> InspectAsync(string gamePath)
    {
        if (!GameDiscovery.IsGameExecutable(gamePath))
            return new(ProxyState.Unavailable, "未设置有效的 Endfield.exe", false, false);
        if (!File.Exists(Source))
            return new(ProxyState.Unavailable, "运行时尚未安装", false, false);
        string target = Path.Combine(Path.GetDirectoryName(gamePath)!, ProxyFileName);
        try
        {
            if (!File.Exists(target))
                return new(ProxyState.NotInstalled, "游戏目录中没有代理。", true, false);
            string sourceHash = await RuntimeService.HashAsync(Source);
            string targetHash = await RuntimeService.HashAsync(target);
            if (sourceHash == targetHash)
                return new(ProxyState.Installed, "已安装，版本与当前运行时一致。", false, true);
            if (IsEfmlProxy(target))
                return new(ProxyState.UpdateAvailable, "已安装旧版 EFML 代理，可以更新。", true, true);
            return new(ProxyState.Conflict,
                "游戏目录里已有其他加载器的 xinput1_4.dll（例如 Better-Endfield 或 EndCraft）。为避免破坏它，EFML 不会覆盖或删除这个文件。",
                false, false);
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
        {
            return new(ProxyState.Unavailable, "无法检查代理：" + exception.Message, false, false);
        }
    }

    public static async Task InstallAsync(string gamePath)
    {
        ProxyStatus status = await InspectAsync(gamePath);
        if (status.State is ProxyState.Conflict or ProxyState.Unavailable) throw new IOException(status.Message);
        string directory = Path.GetDirectoryName(gamePath)!;
        string target = Path.Combine(directory, ProxyFileName);
        string temporary = target + ".efml.tmp";
        string hash = await RuntimeService.HashAsync(Source);
        if (status.State != ProxyState.Installed)
        {
            try
            {
                File.Copy(Source, temporary, true);
                if (await RuntimeService.HashAsync(temporary) != hash) throw new InvalidDataException("代理复制后校验不一致");
                File.Move(temporary, target, true);
            }
            catch (UnauthorizedAccessException)
            {
                throw new UnauthorizedAccessException("没有写入游戏目录的权限。请以管理员身份运行 EFML，或改用注入方式。");
            }
            finally
            {
                TryDelete(temporary);
            }
        }
        await File.WriteAllTextAsync(Path.Combine(directory, ManifestFileName),
            JsonSerializer.Serialize(new Ownership(ProductId, ProxyFileName, hash, DateTimeOffset.UtcNow)));
    }

    public static async Task UninstallAsync(string gamePath)
    {
        string directory = Path.GetDirectoryName(gamePath)!;
        string target = Path.Combine(directory, ProxyFileName);
        if (File.Exists(target))
        {
            bool owned = IsEfmlProxy(target) ||
                (File.Exists(Source) && await RuntimeService.HashAsync(Source) == await RuntimeService.HashAsync(target));
            if (!owned) throw new IOException("游戏目录里的 xinput1_4.dll 不是 EFML 的，已拒绝删除。");
            File.Delete(target);
        }
        TryDelete(Path.Combine(directory, ManifestFileName));
    }

    public static async Task TryUninstallSavedAsync()
    {
        try
        {
            var settings = ManagerSettings.Load();
            if (GameDiscovery.IsGameExecutable(settings.GamePath)) await UninstallAsync(settings.GamePath);
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException or InvalidOperationException)
        {
        }
    }

    private static void TryDelete(string path)
    {
        try
        {
            if (File.Exists(path)) File.Delete(path);
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
        {
        }
    }
}
