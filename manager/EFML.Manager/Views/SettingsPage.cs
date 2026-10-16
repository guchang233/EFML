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

        var mode = Ui.Card(Ui.Column(10,
            Ui.Text("加载组件（xinput1_4.dll 代理）", 16),
            Ui.Hint("EFML 不直接启动游戏。代理装在游戏目录里：从鹰角官方启动器开始游戏时，游戏会自动加载它，再由它加载已启用的模组。安装一次即可。"),
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

        Content = Ui.Page(Ui.Column(16, Ui.Title("设置"), _message, game, mode, look, runtime));
        Loaded += async (_, _) => await RefreshAsync();
    }

    private async Task RefreshAsync()
    {
        _loading = true;
        var settings = _window.Settings;
        _gamePath.Text = settings.GamePath;
        _theme.SelectedIndex = settings.Theme switch { "light" => 1, "dark" => 2, _ => 0 };
        _loading = false;

        var state = RuntimeService.Inspect();
        _runtime.Text = state.Installed ? $"运行时 {AppPaths.Version} 已安装：{state.Directory}" : "运行时未安装。" + state.Problem;
        ProxyStatus proxy = await ProxyService.InspectAsync(settings.GamePath);
        _proxyStatus.Text = proxy.Message;
        _proxyInstall.Content = proxy.State == ProxyState.UpdateAvailable ? "更新代理" : "安装代理到游戏目录";
        _proxyInstall.IsEnabled = proxy.CanInstall;
        _proxyRemove.IsEnabled = proxy.CanUninstall;
    }

    private void Save()
    {
        if (_loading) return;
        var settings = _window.Settings;
        settings.GamePath = _gamePath.Text.Trim().Trim('"');
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
        var picker = new FileOpenPicker();
        picker.FileTypeFilter.Add(".exe");
        InitializeWithWindow.Initialize(picker, WindowNative.GetWindowHandle(_window));
        var file = await picker.PickSingleFileAsync();
        if (file is null) return;
        _gamePath.Text = file.Path;
        Save();
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
