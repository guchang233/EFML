using System.Text.Json;

namespace EFML.Manager.Services;

internal enum LoadMode
{
    Inject,
    Proxy
}

internal sealed class ManagerSettings
{
    public string GamePath { get; set; } = "";
    // Injection writes nothing to the game folder but asks for administrator rights each launch;
    // the proxy lives in the game folder and loads EFML however the game is started.
    public LoadMode LoadMode { get; set; } = LoadMode.Inject;
    public string LaunchArguments { get; set; } = "";
    public string Theme { get; set; } = "system";
    public bool MinimizeAfterLaunch { get; set; } = true;
    public bool FirstRunDone { get; set; }
    public bool PaneOpen { get; set; } = true;

    private static readonly JsonSerializerOptions Options = new() { WriteIndented = true };

    public static ManagerSettings Load()
    {
        try
        {
            if (File.Exists(AppPaths.SettingsFile))
            {
                return JsonSerializer.Deserialize<ManagerSettings>(File.ReadAllText(AppPaths.SettingsFile), Options) ?? new();
            }
        }
        catch (Exception exception) when (exception is IOException or JsonException or UnauthorizedAccessException)
        {
        }
        return new();
    }

    public void Save()
    {
        Directory.CreateDirectory(AppPaths.DataRoot);
        string temporary = AppPaths.SettingsFile + ".tmp";
        File.WriteAllText(temporary, JsonSerializer.Serialize(this, Options));
        File.Move(temporary, AppPaths.SettingsFile, true);
        if (GamePath.Length > 0) AppPaths.WriteIni("Game", "path", GamePath);
    }
}
