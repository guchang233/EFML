using System.Diagnostics;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using Microsoft.Win32;

namespace EFML.Manager.Services;

// Read-only view of the installed Hypergryph launcher used by the direct launch. The native
// EFML.Launcher.exe repeats this discovery and verifies the Authenticode signature before it is
// used as the game's parent; this copy only feeds the settings page (path, signer, version).
internal static class LauncherDiscovery
{
    public sealed record Result(string Path, string Publisher, string Version, string Problem)
    {
        public bool Found => Path.Length > 0;
    }

    private const string GamesCore = "Games.exe";
    private const string LauncherEntry = "Launcher.exe";
    private static readonly string[] PublisherMarkers = ["hypergryph", "gryphline"];
    private static readonly string[] ProductNames =
        ["鹰角启动器", "Hypergryph Launcher", "GRYPHLINK", "Gryphlink", "Gryphline Launcher"];

    public static Result Inspect(string preferred)
    {
        var candidates = Candidates(preferred).ToList();
        bool preferredGiven = !string.IsNullOrWhiteSpace(preferred);
        bool preferredResolved = false;
        for (int index = 0; index < candidates.Count; ++index)
        {
            string? games = ResolveToGamesCore(candidates[index]);
            if (games is null) continue;
            if (index == 0 && preferredGiven) preferredResolved = true;
            string publisher = "", problem = "", version = "";
            try
            {
                version = FileVersionInfo.GetVersionInfo(games).FileVersion ?? "";
            }
            catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
            {
            }
            try
            {
                // X509CertificateLoader handles certificate files, not signed PE images, so the
                // signed-file API stays. This only labels the file for the UI; EFML.Launcher.exe
                // performs the authoritative Authenticode check before using it as a parent.
#pragma warning disable SYSLIB0057
                X509Certificate certificate = X509Certificate.CreateFromSignedFile(games);
#pragma warning restore SYSLIB0057
                publisher = certificate.Subject;
                if (!PublisherMarkers.Any(marker => publisher.Contains(marker, StringComparison.OrdinalIgnoreCase)))
                    problem = "签名者不是鹰角/GRYPHLINE；直接启动会被客户端拒绝。";
            }
            catch (CryptographicException)
            {
                problem = "文件没有可用的 Authenticode 签名；直接启动会被客户端拒绝。";
            }
            catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
            {
                problem = "无法读取签名：" + exception.Message;
            }
            if (preferredGiven && !preferredResolved)
                problem = ("指定的启动器路径无效，已改用自动查找。" + problem).Trim();
            return new Result(Path.GetFullPath(games), publisher, version, problem);
        }
        return new Result("", "", "", preferredGiven
            ? "指定的官方启动器路径无效，自动查找也没有结果。请先安装并启动一次鹰角启动器。"
            : "没有找到官方启动器（Games.exe）。请先安装并启动一次鹰角启动器，或在上方手动指定。");
    }

    private static IEnumerable<string> Candidates(string preferred)
    {
        var emitted = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        IEnumerable<string> Yield(string? candidate)
        {
            if (string.IsNullOrWhiteSpace(candidate)) yield break;
            string full;
            try
            {
                full = Path.GetFullPath(candidate.Trim().Trim('"'));
            }
            catch (Exception exception) when (exception is ArgumentException or NotSupportedException or PathTooLongException)
            {
                yield break;
            }
            if (emitted.Add(full)) yield return full;
        }

        if (!string.IsNullOrWhiteSpace(preferred))
        {
            foreach (string path in Yield(preferred)) yield return path;
        }
        foreach (string root in Roots())
        {
            foreach (string path in Yield(Path.Combine(root, GamesCore))) yield return path;
            foreach (string path in Yield(Path.Combine(root, LauncherEntry))) yield return path;
            foreach (string directory in VersionedDirectories(root))
            {
                foreach (string path in Yield(Path.Combine(directory, GamesCore))) yield return path;
                foreach (string path in Yield(Path.Combine(directory, LauncherEntry))) yield return path;
            }
        }
    }

    // Resolves Launcher.exe or an install folder to the versioned Games.exe the client expects.
    private static string? ResolveToGamesCore(string candidate)
    {
        try
        {
            if (File.Exists(candidate))
            {
                if (Path.GetFileName(candidate).Equals(GamesCore, StringComparison.OrdinalIgnoreCase))
                    return candidate;
                string folder = Path.GetDirectoryName(candidate) ?? "";
                string direct = Path.Combine(folder, GamesCore);
                if (File.Exists(direct)) return direct;
                foreach (string directory in VersionedDirectories(folder))
                {
                    string versioned = Path.Combine(directory, GamesCore);
                    if (File.Exists(versioned)) return versioned;
                }
                return null;
            }
            if (Directory.Exists(candidate))
            {
                string direct = Path.Combine(candidate, GamesCore);
                if (File.Exists(direct)) return direct;
                foreach (string directory in VersionedDirectories(candidate))
                {
                    string versioned = Path.Combine(directory, GamesCore);
                    if (File.Exists(versioned)) return versioned;
                }
            }
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException or
                                               ArgumentException or PathTooLongException)
        {
        }
        return null;
    }

    private static IEnumerable<string> VersionedDirectories(string root)
    {
        string[] directories;
        try
        {
            directories = Directory.Exists(root) ? Directory.GetDirectories(root) : [];
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
        {
            yield break;
        }
        // Newest first, like the native discovery: "1.6.0" before "1.5.3".
        foreach (string directory in directories
            .Select(path => (Path: path, Version: ParseVersion(Path.GetFileName(path))))
            .Where(entry => entry.Version is not null)
            .OrderByDescending(entry => entry.Version, VersionComparer.Instance)
            .Select(entry => entry.Path))
        {
            yield return directory;
        }
    }

    private static int[]? ParseVersion(string name)
    {
        if (name.Length == 0 || name.Any(character => character != '.' && !char.IsAsciiDigit(character)))
            return null;
        string[] parts = name.Split('.', StringSplitOptions.RemoveEmptyEntries);
        var version = new int[parts.Length];
        for (int index = 0; index < parts.Length; ++index)
        {
            if (!int.TryParse(parts[index], out version[index])) return null;
        }
        return version;
    }

    private sealed class VersionComparer : IComparer<int[]?>
    {
        public static readonly VersionComparer Instance = new();

        public int Compare(int[]? left, int[]? right)
        {
            if (left is null || right is null) return 0;
            for (int index = 0; index < Math.Min(left.Length, right.Length); ++index)
            {
                int difference = left[index].CompareTo(right[index]);
                if (difference != 0) return difference;
            }
            return left.Length.CompareTo(right.Length);
        }
    }

    private static List<string> Roots()
    {
        var roots = new List<string>();
        void AddRoot(string? path)
        {
            if (string.IsNullOrWhiteSpace(path)) return;
            string trimmed = path.Trim().Trim('"');
            try
            {
                trimmed = Path.GetFullPath(trimmed);
            }
            catch (Exception exception) when (exception is ArgumentException or NotSupportedException or PathTooLongException)
            {
                return;
            }
            if (trimmed.EndsWith(".exe", StringComparison.OrdinalIgnoreCase))
                trimmed = Path.GetDirectoryName(trimmed) ?? trimmed;
            if (!roots.Contains(trimmed, StringComparer.OrdinalIgnoreCase)) roots.Add(trimmed);
        }

        try
        {
            using RegistryKey classes = Registry.ClassesRoot;
            foreach (string name in classes.GetSubKeyNames())
            {
                if (!name.StartsWith("hypergryphlauncher", StringComparison.OrdinalIgnoreCase) &&
                    !name.StartsWith("gryphlinklauncher", StringComparison.OrdinalIgnoreCase)) continue;
                using RegistryKey? command = classes.OpenSubKey(name + @"\shell\open\command");
                string text = command?.GetValue(null) as string ?? "";
                AddRoot(FirstArgument(text));
            }
        }
        catch (Exception exception) when (exception is UnauthorizedAccessException or System.Security.SecurityException or IOException)
        {
        }

        foreach (RegistryHive hive in new[] { RegistryHive.CurrentUser, RegistryHive.LocalMachine })
        {
            foreach (RegistryView view in new[] { RegistryView.Registry64, RegistryView.Registry32 })
            {
                try
                {
                    using RegistryKey baseKey = RegistryKey.OpenBaseKey(hive, view);
                    using RegistryKey? uninstall = baseKey.OpenSubKey(
                        @"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall");
                    if (uninstall is null) continue;
                    foreach (string name in uninstall.GetSubKeyNames())
                    {
                        using RegistryKey? product = uninstall.OpenSubKey(name);
                        string display = product?.GetValue("DisplayName") as string ?? "";
                        if (!ProductNames.Contains(display, StringComparer.OrdinalIgnoreCase)) continue;
                        AddRoot(product?.GetValue("InstallLocation") as string);
                    }
                }
                catch (Exception exception) when (exception is IOException or UnauthorizedAccessException or
                                                       System.Security.SecurityException)
                {
                }
            }
        }

        foreach (string variable in new[] { "ProgramFiles", "ProgramFiles(x86)", "LOCALAPPDATA" })
        {
            string? value = Environment.GetEnvironmentVariable(variable);
            if (string.IsNullOrWhiteSpace(value)) continue;
            AddRoot(Path.Combine(value, "Hypergryph Launcher"));
            AddRoot(Path.Combine(value, "GRYPHLINK"));
        }
        return roots;
    }

    // "\"E:\\...\\Launcher.exe\" \"%1\"" -> E:\...\Launcher.exe
    private static string FirstArgument(string command)
    {
        string text = command.Trim();
        if (text.Length == 0) return "";
        if (text[0] == '"')
        {
            int end = text.IndexOf('"', 1);
            return end > 1 ? text[1..end] : "";
        }
        int space = text.IndexOf(' ');
        return space < 0 ? text : text[..space];
    }
}
