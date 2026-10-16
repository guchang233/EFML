using System.Runtime.InteropServices;
using System.Text;

namespace EFML.Manager.Services;

// %LOCALAPPDATA%\EFML (or %EFML_HOME%), shared with the native runtime and efml.exe:
//   loader.ini                       [Loader] host=..., load_host=true; [Game] path=...
//   runtime\<version>\               EFML.Host.dll, EFML.Bootstrap.dll, EFML.Injector.exe, xinput1_4.dll, efml.exe
//   modules\index.json, modules\packages\<generation>\
//   logs\EFML.log, logs\loader.log
//   manager.json                     manager settings
internal static class AppPaths
{
    public const string Version = "1.0.1";
    public static readonly string[] RuntimeFiles =
        ["EFML.Host.dll", "EFML.Bootstrap.dll", "EFML.Injector.exe", "xinput1_4.dll", "efml.exe"];

    public static string DataRoot
    {
        get
        {
            string? home = Environment.GetEnvironmentVariable("EFML_HOME");
            return string.IsNullOrWhiteSpace(home)
                ? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "EFML")
                : home;
        }
    }

    public static string ModulesRoot => Path.Combine(DataRoot, "modules");
    public static string IndexPath => Path.Combine(ModulesRoot, "index.json");
    public static string LoaderIni => Path.Combine(DataRoot, "loader.ini");
    public static string LogsRoot => Path.Combine(DataRoot, "logs");
    public static string HostLog => Path.Combine(LogsRoot, "EFML.log");
    public static string LoaderLog => Path.Combine(LogsRoot, "loader.log");
    public static string SettingsFile => Path.Combine(DataRoot, "manager.json");
    public static string InstalledRuntime => Path.Combine(DataRoot, "runtime", Version);

    // Single-file publishing extracts content to a temporary folder; the runtime ships next to the EXE.
    public static string ApplicationDirectory =>
        Path.GetDirectoryName(Environment.ProcessPath) ?? AppContext.BaseDirectory;
    public static string BundledRuntime => Path.Combine(ApplicationDirectory, "runtime");

    public static string ReadIni(string section, string key)
    {
        var buffer = new StringBuilder(32768);
        GetPrivateProfileStringW(section, key, "", buffer, buffer.Capacity, LoaderIni);
        return buffer.ToString();
    }

    public static void WriteIni(string section, string key, string value)
    {
        Directory.CreateDirectory(DataRoot);
        WritePrivateProfileStringW(section, key, value, LoaderIni);
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetPrivateProfileStringW(string section, string key, string fallback,
        StringBuilder value, int size, string file);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool WritePrivateProfileStringW(string section, string key, string value, string file);
}
