using EFML.Manager.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Windows.Storage.Pickers;
using WinRT.Interop;

namespace EFML.Manager.Views;

internal sealed class SettingsPage : UserControl
{
    private readonly MainWindow _window;
    private readonly TextBox _gamePath = new() { PlaceholderText = @"例如 D:\Arknights Endfield\Endfield.exe", MinWidth = 420 };
    private readonly TextBox _launcherPath = new()
    {
        PlaceholderText = @"留空 = 自动查找（例如 E:\Program Files\Hypergryph Launcher\1.6.0\Games.exe）",
        MinWidth = 420
    };
    private readonly TextBlock _launcherStatus = Ui.Hint("");
    private readonly TextBlock _proxyStatus = Ui.Text("");
    private readonly Button _proxyInstall;
    private readonly Button _proxyRemove;
    private readonly ComboBox _theme = new() { MinWidth = 160 };
    private readonly TextBlock _runtime = Ui.Text("");
    private readonly InfoBar _message = new() { IsClosable = true };
    private bool _loading;

    public SettingsPage(MainWindow window)
    {
        _window = window;
        _proxyInstall = Ui.Button("安装代理到游戏目录", async () => await ProxyActionAsync(true));
        _proxyRemove = Ui.Button("从游戏目录移除代理", async () => await ProxyActionAsync(false));

        _gamePath.LostFocus += (_, _) => Save();
        _launcherPath.LostFocus += (_, _) => Save();
        foreach (var (text, tag) in new[] { ("跟随系统", "system"), ("浅色", "light"), ("深色", "dark") })
            _theme.Items.Add(new ComboBoxItem { Content = text, Tag = tag });
        _theme.SelectionChanged += (_, _) => Save();

        var game = Ui.Card(Ui.Column(10,
            Ui.Text("游戏位置", 16),
            Ui.Row(_gamePath,
                Ui.Button("浏览…", async () => await BrowseAsync()),
                Ui.Button("自动查找", () =>
                {
                    string found = GameDiscovery.Discover("");
                    if (found.Length > 0) { _gamePath.Text = found; Save(); }
                    else Ui.Show(_message, "没有找到 Endfield.exe，请手动选择。", InfoBarSeverity.Warning);
                })),
            Ui.Hint("EFML 会在注册表与常见安装位置中查找终末地。")));

        var launcher = Ui.Card(Ui.Column(10,
            Ui.Text("官方启动器（直接启动用）", 16),
            Ui.Hint("「直接启动游戏」会用一个真实的官方 Games.exe 暂停进程作为游戏的父进程，所以不需要打开启动器界面；" +
                    "游戏仍然通过代理加载 EFML。留空表示自动查找。"),
            Ui.Row(_launcherPath,
                Ui.Button("浏览…", async () => await BrowseLauncherAsync()),
                Ui.Button("自动查找", () =>
                {
                    LauncherDiscovery.Result found = LauncherDiscovery.Inspect("");
                    if (found.Found) { _launcherPath.Text = found.Path; Save(); }
                    else Ui.Show(_message, found.Problem, InfoBarSeverity.Warning);
                }),
                Ui.Button("检查", () => UpdateLauncherStatus(true))),
            _launcherStatus));

        var mode = Ui.Card(Ui.Column(10,
            Ui.Text("加载组件（xinput1_4.dll 代理）", 16),
            Ui.Hint("代理装在游戏目录里：游戏启动时自动加载它，再由它加载已启用的模组。安装一次即可；" +
                    "直启和官方启动器两条路径都走它。"),
            Ui.Text("代理状态", 14),
            _proxyStatus,
            Ui.Row(_proxyInstall, _proxyRemove)));

        var look = Ui.Card(Ui.Column(10, Ui.Text("外观", 16), _theme));

        var runtime = Ui.Card(Ui.Column(10,
            Ui.Text("运行时与数据", 16),
            _runtime,
            Ui.Row(
                Ui.Button("重新安装运行时", async () =>
                {
                    try
                    {
                        await RuntimeService.InstallAsync();
                        Ui.Show(_message, "运行时已安装。", InfoBarSeverity.Success);
                    }
                    catch (Exception e)
                    {
                        Ui.Show(_message, e.Message, InfoBarSeverity.Error);
                    }
                    await RefreshAsync();
                }),
                Ui.Button("打开数据目录", () => Shell.OpenFolder(AppPaths.DataRoot))),
            Ui.Hint("模组、日志和配置都在数据目录中；卸载 EFML 不会删除它们。设置环境变量 EFML_HOME 可以改用其他目录。")));

        Content = Ui.Page(Ui.Column(16, Ui.Title("设置"), _message, game, launcher, mode, look, runtime));
        Loaded += async (_, _) => await RefreshAsync();
    }

    private async Task RefreshAsync()
    {
        _loading = true;
        var settings = _window.Settings;
        _gamePath.Text = settings.GamePath;
        _launcherPath.Text = settings.OfficialLauncherPath;
        _theme.SelectedIndex = settings.Theme switch { "light" => 1, "dark" => 2, _ => 0 };
        _loading = false;

        var state = RuntimeService.Inspect();
        _runtime.Text = state.Installed ? $"运行时 {AppPaths.Version} 已安装：{state.Directory}" : "运行时未安装。" + state.Problem;
        ProxyStatus proxy = await ProxyService.InspectAsync(settings.GamePath);
        _proxyStatus.Text = proxy.Message;
        _proxyInstall.Content = proxy.State == ProxyState.UpdateAvailable ? "更新代理" : "安装代理到游戏目录";
        _proxyInstall.IsEnabled = proxy.CanInstall;
        _proxyRemove.IsEnabled = proxy.CanUninstall;
        UpdateLauncherStatus(false);
    }

    private void Save()
    {
        if (_loading) return;
        var settings = _window.Settings;
        settings.GamePath = _gamePath.Text.Trim().Trim('"');
        settings.OfficialLauncherPath = _launcherPath.Text.Trim().Trim('"');
        settings.Theme = (_theme.SelectedItem as ComboBoxItem)?.Tag as string ?? "system";
        try
        {
            settings.Save();
        }
        catch (Exception e)
        {
            Ui.Show(_message, "无法保存设置：" + e.Message, InfoBarSeverity.Error);
        }
        _window.ApplyTheme();
        if (settings.GamePath.Length > 0 && !GameDiscovery.IsGameExecutable(settings.GamePath))
            Ui.Show(_message, "这个路径不是 Endfield.exe。", InfoBarSeverity.Warning);
        _ = RefreshProxyAsync();
        UpdateLauncherStatus(false);
    }

    private void UpdateLauncherStatus(bool announce)
    {
        LauncherDiscovery.Result result = LauncherDiscovery.Inspect(_window.Settings.OfficialLauncherPath);
        if (!result.Found)
        {
            _launcherStatus.Text = result.Problem;
            if (announce) Ui.Show(_message, result.Problem, InfoBarSeverity.Warning);
            return;
        }
        string text = $"已找到：{result.Path}\n版本 {result.Version} · 签名者 {result.Publisher}";
        if (result.Problem.Length > 0) text += "\n⚠ " + result.Problem;
        _launcherStatus.Text = text;
        if (announce)
        {
            Ui.Show(_message, result.Problem.Length > 0 ? result.Problem : "官方启动器可用。",
                result.Problem.Length > 0 ? InfoBarSeverity.Warning : InfoBarSeverity.Success);
        }
    }

    private async Task RefreshProxyAsync()
    {
        ProxyStatus proxy = await ProxyService.InspectAsync(_window.Settings.GamePath);
        _proxyStatus.Text = proxy.Message;
        _proxyInstall.IsEnabled = proxy.CanInstall;
        _proxyRemove.IsEnabled = proxy.CanUninstall;
    }

    private async Task BrowseAsync()
    {
        string? path = await PickExecutableAsync();
        if (path is null) return;
        _gamePath.Text = path;
        Save();
    }

    private async Task BrowseLauncherAsync()
    {
        string? path = await PickExecutableAsync();
        if (path is null) return;
        _launcherPath.Text = path;
        Save();
    }

    private async Task<string?> PickExecutableAsync()
    {
        var picker = new FileOpenPicker();
        picker.FileTypeFilter.Add(".exe");
        InitializeWithWindow.Initialize(picker, WindowNative.GetWindowHandle(_window));
        var file = await picker.PickSingleFileAsync();
        return file?.Path;
    }

    private async Task ProxyActionAsync(bool install)
    {
        try
        {
            if (LaunchService.GameRunning()) throw new InvalidOperationException("请先关闭游戏。");
            if (install)
            {
                await RuntimeService.InstallAsync();
                await ProxyService.InstallAsync(_window.Settings.GamePath);
                Ui.Show(_message, "代理已安装。以后无论从哪里启动游戏都会加载 EFML。", InfoBarSeverity.Success);
            }
            else
            {
                await ProxyService.UninstallAsync(_window.Settings.GamePath);
                Ui.Show(_message, "代理已移除。", InfoBarSeverity.Success);
            }
        }
        catch (Exception e)
        {
            Ui.Show(_message, e.Message, InfoBarSeverity.Error);
        }
        await RefreshAsync();
    }
}
