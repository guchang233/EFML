using EFML.Manager.Services;
using Microsoft.UI.Xaml;

namespace EFML.Manager;

public partial class App : Application
{
    public static MainWindow? MainWindowInstance => (Current as App)?._window;
    private MainWindow? _window;

    public App()
    {
        UnhandledException += (_, e) => CrashLog("UnhandledException", e.Exception);
        InitializeComponent();
    }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        string[] arguments = Environment.GetCommandLineArgs().Skip(1).ToArray();
        // The installer's uninstaller removes the proxy EFML put in the game folder.
        if (arguments.Any(a => a.Equals("--uninstall-game-files-silent", StringComparison.OrdinalIgnoreCase)))
        {
            Task.Run(ProxyService.TryUninstallSavedAsync).GetAwaiter().GetResult();
            Exit();
            return;
        }
        try
        {
            _window = new MainWindow();
            _window.Activate();
        }
        catch (Exception exception)
        {
            CrashLog("OnLaunched", exception);
            throw;
        }
    }

    internal static void CrashLog(string where, Exception? exception)
    {
        try
        {
            Directory.CreateDirectory(AppPaths.LogsRoot);
            File.AppendAllText(Path.Combine(AppPaths.LogsRoot, "manager-crash.log"),
                $"{DateTime.Now:yyyy-MM-dd HH:mm:ss} {where}: {exception}\r\n");
        }
        catch
        {
        }
    }
}
