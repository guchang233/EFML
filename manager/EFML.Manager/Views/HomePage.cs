using EFML.Manager.Services;
using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using System.Text;
using System.Text.Json.Nodes;

namespace EFML.Manager.Views;

// Launch page: start the game directly (the official launcher's identity without its UI), or prepare
// the proxy and guide the user through the official launcher. Live state of the game, the Host, the
// modules and the Host / loader logs.
internal sealed class HomePage : UserControl
{
    private readonly MainWindow _window;
    private readonly Ui.Tile _game = new("游戏");
    private readonly Ui.Tile _host = new("EFML 宿主");
    private readonly Ui.Tile _thread = new("游戏线程");
    private readonly Ui.Tile _modules = new("模组");
    private readonly Button _launch = new() { MinWidth = 180, Height = 44 };
    private readonly Button _official = new() { Height = 44 };
    private readonly TextBlock _mode = Ui.Hint("");
    private readonly TextBlock _gamePath = Ui.Hint("");
    private readonly InfoBar _message = new() { IsClosable = true };
    private readonly ComboBox _logSource = new() { MinWidth = 200 };
    private readonly CheckBox _follow = new() { Content = "自动滚动", IsChecked = true };
    private readonly TextBox _log = new()
    {
        IsReadOnly = true, AcceptsReturn = true, TextWrapping = TextWrapping.NoWrap, Height = 360,
        FontFamily = new FontFamily("Cascadia Mono, Consolas"), FontSize = 12
    };
    private readonly DispatcherTimer _timer = new() { Interval = TimeSpan.FromMilliseconds(1500) };
    private bool _refreshing, _launching;
    private string _logText = "";

    public HomePage(MainWindow window)
    {
        _window = window;
        _launch.Style = (Style)Application.Current.Resources["AccentButtonStyle"];
        _launch.Content = new TextBlock { Text = "直接启动游戏", FontSize = 16, FontWeight = FontWeights.SemiBold };
        _launch.Click += async (_, _) => await LaunchAsync();
        _official.Content = "用官方启动器启动";
        _official.Click += async (_, _) => await GuideAsync();
        ScrollViewer.SetHorizontalScrollBarVisibility(_log, ScrollBarVisibility.Auto);
        ScrollViewer.SetVerticalScrollBarVisibility(_log, ScrollBarVisibility.Auto);

        var tiles = new Grid { ColumnSpacing = 12 };
        foreach (var tile in new[] { _game, _host, _thread, _modules })
        {
            tiles.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            Grid.SetColumn(tile.Element, tiles.Children.Count);
            tiles.Children.Add(tile.Element);
        }

        var launchGrid = new Grid { ColumnSpacing = 16 };
        launchGrid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        launchGrid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var launchText = Ui.Column(4, Ui.Text("启动终末地", 16), _mode, _gamePath);
        launchGrid.Children.Add(launchText);
        var launchButtons = Ui.Row(_launch, _official);
        launchButtons.VerticalAlignment = VerticalAlignment.Center;
        Grid.SetColumn(launchButtons, 1);
        launchGrid.Children.Add(launchButtons);

        _logSource.Items.Add(new ComboBoxItem { Content = "宿主日志（EFML.log）", Tag = "host" });
        _logSource.Items.Add(new ComboBoxItem { Content = "加载器日志（loader.log）", Tag = "loader" });
        _logSource.SelectedIndex = 0;
        _logSource.SelectionChanged += (_, _) => { _logText = ""; RefreshLog(); };
        var logHeader = new Grid();
        logHeader.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        logHeader.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        logHeader.Children.Add(Ui.Section("实时日志"));
        var logTools = Ui.Row(_logSource, _follow, Ui.Button("打开日志文件夹", () => Shell.OpenFolder(AppPaths.LogsRoot)));
        logTools.VerticalAlignment = VerticalAlignment.Bottom;
        Grid.SetColumn(logTools, 1);
        logHeader.Children.Add(logTools);

        Content = Ui.Page(Ui.Column(16,
            Ui.Title("启动"),
            Ui.Hint("EFML 在游戏启动时加载。先在「模组」里安装并启用模组，再启动游戏：直接启动不需要打开鹰角启动器。"),
            tiles,
            Ui.Card(launchGrid, 20),
            _message,
            logHeader,
            _log));

        _timer.Tick += async (_, _) => await RefreshAsync();
        Loaded += async (_, _) => { _timer.Start(); await RefreshAsync(); };
        Unloaded += (_, _) => _timer.Stop();
    }

    // Direct launch: EFML.Launcher.exe starts Endfield.exe with the installed official launcher as
    // its parent. The launcher UI never appears; EFML still loads through the proxy.
    private async Task LaunchAsync()
    {
        if (_launching) return;
        _launching = true;
        _launch.IsEnabled = false;
        _official.IsEnabled = false;
        Ui.Show(_message, "正在准备运行时和加载组件…", InfoBarSeverity.Informational);
        try
        {
            LaunchService.LaunchResult result = await LaunchService.LaunchDirectAsync(_window.Settings);
            Ui.Show(_message, result.Message,
                result.Started ? InfoBarSeverity.Success : InfoBarSeverity.Warning);
        }
        catch (Exception exception)
        {
            Ui.Show(_message, exception.Message, InfoBarSeverity.Error);
        }
        finally
        {
            _launching = false;
            await RefreshAsync();
        }
    }

    // Guided launch: make sure the runtime and the proxy are in place, then let the user click Start
    // Game in the official launcher.
    private async Task GuideAsync()
    {
        if (_launching) return;
        _launching = true;
        _launch.IsEnabled = false;
        _official.IsEnabled = false;
        Ui.Show(_message, "正在准备加载组件…", InfoBarSeverity.Informational);
        try
        {
            await LaunchService.PrepareAsync(_window.Settings);
            _message.IsOpen = false;
            await ShowGuideAsync();
        }
        catch (Exception exception)
        {
            Ui.Show(_message, exception.Message, InfoBarSeverity.Error);
        }
        finally
        {
            _launching = false;
            await RefreshAsync();
        }
    }

    private async Task ShowGuideAsync()
    {
        bool running = LaunchService.GameRunning();
        var dialog = new ContentDialog
        {
            XamlRoot = XamlRoot,
            Title = "请用官方启动器启动游戏",
            Content = Ui.Column(10,
                Ui.Text("EFML 的加载组件已就绪。接下来由鹰角官方启动器创建游戏进程。"),
                Ui.Text(running
                    ? "检测到游戏正在运行：它不是用已就绪的组件启动的。请先关闭游戏，再按下面的步骤重新开始。"
                    : "接下来："),
                Ui.Text("1. 打开鹰角启动器（桌面或开始菜单里的快捷方式）。"),
                Ui.Text("2. 在启动器中点击「开始游戏」。"),
                Ui.Text("3. 回到 EFML 的这个页面：游戏、宿主、游戏线程和模组的状态会在进入游戏后依次变为运行中。"),
                Ui.Hint("也可以直接用「直接启动游戏」：EFML 会借用本机官方启动器的身份拉起游戏，界面不会出现，但需要一次管理员授权。")),
            CloseButtonText = "知道了",
            DefaultButton = ContentDialogButton.Close
        };
        await dialog.ShowAsync();
    }

    public async Task RefreshAsync()
    {
        if (_refreshing) return;
        _refreshing = true;
        try
        {
            var settings = _window.Settings;
            _mode.Text = "直接启动会借用本机官方启动器的身份（不显示其界面，需要一次管理员授权）；" +
                "也可以点「用官方启动器启动」，在启动器里开始游戏。两条路径都通过代理加载模组。";
            _gamePath.Text = GameDiscovery.IsGameExecutable(settings.GamePath) ? settings.GamePath : "未找到游戏，请到「设置」中选择 Endfield.exe";

            bool running = LaunchService.GameRunning();
            JsonObject? status = running ? await ModuleService.HostStatusAsync() : null;
            _game.Set(running ? "运行中" : "未运行", running ? "" : "点「直接启动游戏」，或从官方启动器开始", running ? "EfmlOkBrush" : "TextFillColorTertiaryBrush");
            _launch.IsEnabled = !_launching;
            _official.IsEnabled = !_launching;

            JsonObject? host = status?["host"] as JsonObject;
            if (host is null)
            {
                _host.Set(running ? "未加载" : "—", running ? "宿主还在启动，或加载组件未安装（点「直接启动游戏」检查）" : "", running ? "EfmlWarnBrush" : "TextFillColorTertiaryBrush");
                _thread.Set("—", "", "TextFillColorTertiaryBrush");
            }
            else
            {
                bool ready = host["runtime_ready"]?.GetValue<bool>() ?? false;
                _host.Set(ready ? "已就绪" : "等待游戏运行时", $"EFML {host["version"]} · 进程 {host["pid"]}", ready ? "EfmlOkBrush" : "EfmlWarnBrush");
                var thread = host["game_thread"] as JsonObject;
                bool threadReady = thread?["ready"]?.GetValue<bool>() ?? false;
                _thread.Set(threadReady ? "运行中" : "等待中",
                    threadReady ? $"{thread!["source"]} · {thread["frames"]} 帧" : "进入游戏场景后开始", threadReady ? "EfmlOkBrush" : "EfmlWarnBrush");
            }

            int enabled = 0, readyCount = 0, failed = 0;
            try
            {
                enabled = ModuleService.Records().Count(r => r.Enabled);
            }
            catch (Exception)
            {
            }
            if (status?["modules"] is JsonArray modules)
            {
                readyCount = modules.Count(m => m?["status"]?.GetValue<string>() == "ready");
                failed = modules.Count(m => m?["status"]?.GetValue<string>() == "failed");
                _modules.Set($"{readyCount} / {enabled} 运行中", failed > 0 ? $"{failed} 个失败，详见「模组」" : "已启用的模组",
                    failed > 0 ? "EfmlErrorBrush" : readyCount == enabled ? "EfmlOkBrush" : "EfmlWarnBrush");
            }
            else
            {
                _modules.Set($"{enabled} 个已启用", "游戏启动后加载", enabled > 0 ? "EfmlAccentBrush" : "TextFillColorTertiaryBrush");
            }
            RefreshLog();
        }
        finally
        {
            _refreshing = false;
        }
    }

    private void RefreshLog()
    {
        string file = (_logSource.SelectedItem as ComboBoxItem)?.Tag as string == "loader" ? AppPaths.LoaderLog : AppPaths.HostLog;
        string text = Tail(file, 400);
        if (text == _logText) return;
        _logText = text;
        _log.Text = text.Length > 0 ? text : "（还没有日志。启动游戏后，这里会实时显示宿主和模组的输出。）";
        if (_follow.IsChecked == true)
        {
            _log.SelectionStart = _log.Text.Length;
            _log.DispatcherQueue.TryEnqueue(() =>
            {
                if (FindScrollViewer(_log) is ScrollViewer viewer) viewer.ChangeView(null, viewer.ScrollableHeight, null, true);
            });
        }
    }

    private static ScrollViewer? FindScrollViewer(DependencyObject root)
    {
        for (int i = 0; i < VisualTreeHelper.GetChildrenCount(root); i++)
        {
            var child = VisualTreeHelper.GetChild(root, i);
            if (child is ScrollViewer viewer) return viewer;
            if (FindScrollViewer(child) is ScrollViewer nested) return nested;
        }
        return null;
    }

    internal static string Tail(string path, int lines)
    {
        try
        {
            if (!File.Exists(path)) return "";
            using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            long start = Math.Max(0, stream.Length - 256 * 1024);
            stream.Seek(start, SeekOrigin.Begin);
            using var reader = new StreamReader(stream, Encoding.UTF8);
            string[] all = reader.ReadToEnd().Replace("\r\n", "\n").Split('\n');
            return string.Join("\r\n", all.Skip(Math.Max(0, all.Length - lines)).Where(l => l.Length > 0));
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
        {
            return "";
        }
    }
}
