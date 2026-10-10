using System.Security.Cryptography;

namespace EFML.Manager.Services;

// Keeps %LOCALAPPDATA%\EFML\runtime\<version>\ in step with the runtime shipped next to the
// manager, and points loader.ini at it. A game that is running keeps the Host locked; the files
// in place are then the same version and are left alone.
internal static class RuntimeService
{
    public sealed record State(bool Bundled, bool Installed, string Directory, string Problem);

    public static State Inspect()
    {
        bool bundled = AppPaths.RuntimeFiles.All(f => File.Exists(Path.Combine(AppPaths.BundledRuntime, f)));
        bool installed = AppPaths.RuntimeFiles.All(f => File.Exists(Path.Combine(AppPaths.InstalledRuntime, f)));
        string host = AppPaths.ReadIni("Loader", "host");
        bool pointed = host.Equals(Path.Combine(AppPaths.InstalledRuntime, "EFML.Host.dll"), StringComparison.OrdinalIgnoreCase);
        string problem = !bundled && !installed ? "管理器旁边没有 runtime 文件夹，请重新安装 EFML" :
            installed && !pointed ? "loader.ini 指向了其他运行时" : "";
        return new State(bundled, installed && pointed, AppPaths.InstalledRuntime, problem);
    }

    public static async Task InstallAsync()
    {
        Directory.CreateDirectory(AppPaths.InstalledRuntime);
        if (Directory.Exists(AppPaths.BundledRuntime))
        {
            foreach (string file in AppPaths.RuntimeFiles)
            {
                string source = Path.Combine(AppPaths.BundledRuntime, file);
                string target = Path.Combine(AppPaths.InstalledRuntime, file);
                if (!File.Exists(source)) throw new FileNotFoundException("运行时文件缺失", source);
                if (File.Exists(target) && await HashAsync(source) == await HashAsync(target)) continue;
                try
                {
                    File.Copy(source, target, true);
                }
                catch (IOException) when (File.Exists(target))
                {
                    throw new IOException($"{file} 正被游戏占用。请关闭游戏后再更新运行时。");
                }
            }
        }
        foreach (string file in AppPaths.RuntimeFiles)
        {
            if (!File.Exists(Path.Combine(AppPaths.InstalledRuntime, file)))
                throw new FileNotFoundException("运行时文件缺失", file);
        }
        AppPaths.WriteIni("Loader", "host", Path.Combine(AppPaths.InstalledRuntime, "EFML.Host.dll"));
        AppPaths.WriteIni("Loader", "load_host", "true");
        Directory.CreateDirectory(AppPaths.LogsRoot);
        await ModuleService.EnsureIndexAsync();
    }

    public static async Task<string> HashAsync(string path)
    {
        await using FileStream stream = new(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
        return Convert.ToHexString(await SHA256.HashDataAsync(stream));
    }
}
