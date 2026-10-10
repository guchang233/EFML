using Microsoft.Win32;

namespace EFML.Manager.Services;

// Derived from Better-Endfield's RuntimePathDiscoveryService (AGPL-3.0).
internal static class GameDiscovery
{
    private const string GameExecutableName = "Endfield.exe";

    public static string Discover(string preferredPath)
    {
        foreach (string candidate in Candidates(preferredPath))
        {
            if (IsGameExecutable(candidate)) return Path.GetFullPath(candidate);
        }
        return "";
    }

    public static bool IsGameExecutable(string path) =>
        !string.IsNullOrWhiteSpace(path) && File.Exists(path) &&
        Path.GetFileName(path).Equals(GameExecutableName, StringComparison.OrdinalIgnoreCase);

    private static IEnumerable<string> Candidates(string preferredPath)
    {
        var emitted = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        IEnumerable<string> Add(string candidate)
        {
            if (string.IsNullOrWhiteSpace(candidate)) yield break;
            string full;
            try
            {
                full = Path.GetFullPath(candidate.Trim().Trim('"'));
            }
            catch (Exception exception) when (exception is ArgumentException or NotSupportedException)
            {
                yield break;
            }
            if (emitted.Add(full)) yield return full;
        }

        foreach (string path in Add(preferredPath)) yield return path;
        foreach (string path in Add(AppPaths.ReadIni("Game", "path"))) yield return path;
        foreach (string candidate in UninstallRegistryCandidates())
        {
            foreach (string path in Add(candidate)) yield return path;
        }
        foreach (DriveInfo drive in DriveInfo.GetDrives())
        {
            bool usable;
            try
            {
                usable = drive.IsReady && drive.DriveType == DriveType.Fixed;
            }
            catch (IOException)
            {
                usable = false;
            }
            if (!usable) continue;
            string root = drive.RootDirectory.FullName;
            foreach (string folder in new[] { root, Path.Combine(root, "Games"), Path.Combine(root, "Game"),
                         Path.Combine(root, "Program Files"), Path.Combine(root, "Program Files (x86)") })
            {
                foreach (string candidate in NamedGameDirectories(folder))
                {
                    foreach (string path in Add(candidate)) yield return path;
                }
            }
        }
    }

    private static IEnumerable<string> NamedGameDirectories(string root)
    {
        if (!Directory.Exists(root)) yield break;
        string[] directories;
        try
        {
            directories = Directory.EnumerateDirectories(root).Take(512).ToArray();
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
        {
            yield break;
        }
        foreach (string directory in directories)
        {
            string name = Path.GetFileName(directory);
            if (!name.Contains("endfield", StringComparison.OrdinalIgnoreCase) &&
                !name.Contains("终末地", StringComparison.OrdinalIgnoreCase)) continue;
            yield return Path.Combine(directory, GameExecutableName);
            yield return Path.Combine(directory, "Endfield Game", GameExecutableName);
            yield return Path.Combine(directory, "games", "Endfield Game", GameExecutableName);
        }
    }

    private static List<string> UninstallRegistryCandidates()
    {
        const string uninstallKey = @"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall";
        var candidates = new List<string>();
        foreach (RegistryHive hive in new[] { RegistryHive.CurrentUser, RegistryHive.LocalMachine })
        {
            foreach (RegistryView view in new[] { RegistryView.Registry64, RegistryView.Registry32 })
            {
                try
                {
                    using RegistryKey baseKey = RegistryKey.OpenBaseKey(hive, view);
                    using RegistryKey? uninstall = baseKey.OpenSubKey(uninstallKey);
                    if (uninstall is null) continue;
                    foreach (string name in uninstall.GetSubKeyNames())
                    {
                        using RegistryKey? product = uninstall.OpenSubKey(name);
                        string displayName = product?.GetValue("DisplayName") as string ?? "";
                        if (!displayName.Contains("Endfield", StringComparison.OrdinalIgnoreCase) &&
                            !displayName.Contains("终末地", StringComparison.OrdinalIgnoreCase)) continue;
                        string location = product?.GetValue("InstallLocation") as string ?? "";
                        if (!string.IsNullOrWhiteSpace(location)) candidates.Add(Path.Combine(location, GameExecutableName));
                        string icon = (product?.GetValue("DisplayIcon") as string ?? "").Split(',', 2)[0].Trim().Trim('"');
                        if (!string.IsNullOrWhiteSpace(icon)) candidates.Add(icon);
                    }
                }
                catch (Exception exception) when (exception is IOException or UnauthorizedAccessException or
                                                      System.Security.SecurityException)
                {
                }
            }
        }
        return candidates;
    }
}
