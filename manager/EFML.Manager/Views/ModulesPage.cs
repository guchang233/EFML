using EFML.Manager.Services;
using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using System.Text.Json.Nodes;
using Windows.Storage.Pickers;
using WinRT.Interop;

namespace EFML.Manager.Views;

// Installed modules. Derived from Better-Endfield's ThirdPartyModulesPage (AGPL-3.0): import,
// enable, order, open the module's page and remove; EFML adds live status from the running
// game, folder import and development folders.
internal sealed class ModulesPage : UserControl
{
    private readonly MainWindow _window;
    private readonly StackPanel _cards = new() { Spacing = 12 };
    private readonly InfoBar _message = new() { IsClosable = true };
    private readonly TextBlock _summary = Ui.Hint("");
    private readonly DispatcherTimer _timer = new() { Interval = TimeSpan.FromSeconds(2) };
    private readonly HashSet<string> _expanded = new(StringComparer.Ordinal);
    private bool _busy;
    private string _lastLive = "";

    public ModulesPage(MainWindow window)
    {
        _window = window;
        var import = Ui.Button("导入模组压缩包", async () => await PickAsync(false), accent: true);
        var folder = Ui.Button("导入文件夹", async () => await PickAsync(true));
        var dev = Ui.Button("开发：注册文件夹", async () => await PickDevelopmentAsync());
        ToolTipService.SetToolTip(dev, "不复制，直接从该文件夹加载模组。重新编译 DLL 后重启游戏即可生效。");
        var open = Ui.Button("打开模组目录", () => Shell.OpenFolder(ModuleService.Root));
        Content = Ui.Page(Ui.Column(16,
            Ui.Title("模组"),
            Ui.Hint("支持 EFML 模组，以及为 Better-Endfield / Better Endfield Next 制作的第三方模块（同一种 module.json 包格式）。启用、停用和配置会实时生效；安装新版本或移除后需要重启游戏。"),
            Ui.Row(import, folder, dev, open),
            _summary,
            _message,
            _cards));
        _timer.Tick += async (_, _) => await RefreshLiveAsync();
        Loaded += async (_, _) => { Render(); _timer.Start(); await RefreshLiveAsync(); };
        Unloaded += (_, _) => _timer.Stop();
    }

    private JsonObject? _live;

    private async Task RefreshLiveAsync()
    {
        _live = LaunchService.GameRunning() ? await ModuleService.HostStatusAsync() : null;
        string signature = _live?["modules"]?.ToJsonString() ?? "";
        if (signature == _lastLive) return;
        _lastLive = signature;
        if (!_busy) Render();
    }

    private JsonObject? Live(string id) =>
        (_live?["modules"] as JsonArray)?.FirstOrDefault(m => m?["id"]?.GetValue<string>() == id) as JsonObject;

    private static (string Text, string Brush) Describe(ModuleRecord record, JsonObject? live)
    {
        if (record.Error.Length > 0) return ("无法读取", "EfmlErrorBrush");
        if (live is null) return record.Enabled ? ("已启用", "EfmlAccentBrush") : ("已停用", "TextFillColorTertiaryBrush");
        if (live["restart_required"]?.GetValue<bool>() == true) return ("需要重启游戏", "EfmlWarnBrush");
        return (live["status"]?.GetValue<string>() ?? "") switch
        {
            "ready" => ("运行中", "EfmlOkBrush"),
            "ui_only" => ("仅页面", "EfmlOkBrush"),
            "waiting_runtime" => ("等待游戏运行时", "EfmlWarnBrush"),
            "waiting_dependencies" => ("等待依赖", "EfmlWarnBrush"),
            "retrying" => ("重试中", "EfmlWarnBrush"),
            "failed" => ("失败", "EfmlErrorBrush"),
            _ => ("已停用", "TextFillColorTertiaryBrush")
        };
    }

    private void Render()
    {
        _cards.Children.Clear();
        IReadOnlyList<ModuleRecord> records;
        try
        {
            records = ModuleService.Records();
        }
        catch (Exception e)
        {
            Ui.Show(_message, "无法读取模组列表：" + e.Message, InfoBarSeverity.Error);
            return;
        }
        _summary.Text = records.Count == 0
            ? ""
            : $"共 {records.Count} 个模组，{records.Count(r => r.Enabled)} 个已启用" + (_live is null ? "（游戏未运行）" : "");
        if (records.Count == 0)
        {
            _cards.Children.Add(Ui.Card(Ui.Column(8,
                Ui.Text("还没有安装模组", 16),
                Ui.Hint("点击「导入模组压缩包」选择模组的 .zip。模组包的根目录必须有 module.json。"))));
            return;
        }
        for (int i = 0; i < records.Count; i++) _cards.Children.Add(Card(records[i], i, records.Count));
    }

    private Border Card(ModuleRecord record, int position, int count)
    {
        JsonObject? live = Live(record.Id);
        var (stateText, stateBrush) = Describe(record, live);

        var name = new TextBlock { Text = record.Name, FontSize = 18, FontWeight = FontWeights.SemiBold, TextWrapping = TextWrapping.Wrap };
        var meta = Ui.Hint(string.Join(" · ", new[] { record.Version, record.Author, record.Id, record.Dev ? "开发文件夹" : "" }.Where(s => s.Length > 0)));
        var chip = new Border
        {
            Background = Ui.Brush(stateBrush), CornerRadius = new CornerRadius(4), Padding = new Thickness(8, 2, 8, 2),
            VerticalAlignment = VerticalAlignment.Top,
            Child = new TextBlock { Text = stateText, FontSize = 12, Foreground = Ui.Brush("TextOnAccentFillColorPrimaryBrush") }
        };
        var toggle = new ToggleSwitch { IsOn = record.Enabled, IsEnabled = record.Supported && !_busy, OnContent = "启用", OffContent = "停用", MinWidth = 0 };
        toggle.Toggled += async (_, _) =>
        {
            if (_busy || toggle.IsOn == record.Enabled) return;
            await ActAsync(() => ModuleService.SetEnabledAsync(record.Id, toggle.IsOn), toggle.IsOn ? $"已启用 {record.Name}" : $"已停用 {record.Name}");
        };

        var header = new Grid { ColumnSpacing = 12 };
        header.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        header.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        header.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        header.Children.Add(Ui.Column(2, name, meta));
        Grid.SetColumn(chip, 1);
        header.Children.Add(chip);
        Grid.SetColumn(toggle, 2);
        header.Children.Add(toggle);

        var body = Ui.Column(10, header);
        if (record.Description.Length > 0) body.Children.Add(Ui.Text(record.Description));
        string error = record.Error.Length > 0 ? record.Error : live?["error"]?.GetValue<string>() ?? "";
        if (error.Length > 0)
        {
            body.Children.Add(new TextBlock { Text = error, TextWrapping = TextWrapping.Wrap, Foreground = Ui.Brush("EfmlErrorBrush") });
        }

        var actions = Ui.Row();
        if (record.Ui.Length > 0)
        {
            actions.Children.Add(Ui.Button("打开模组页面", () =>
            {
                try
                {
                    ModuleWindow.Open(record);
                }
                catch (Exception e)
                {
                    Ui.Show(_message, e.Message, InfoBarSeverity.Error);
                }
            }, accent: true));
        }
        actions.Children.Add(Ui.Button("定位文件", () => Shell.OpenFolder(record.Directory)));
        var up = Ui.Button("上移", async () => await ActAsync(() => ModuleService.MoveAsync(record.Id, -1), "已调整加载顺序"));
        up.IsEnabled = position > 0;
        var down = Ui.Button("下移", async () => await ActAsync(() => ModuleService.MoveAsync(record.Id, 1), "已调整加载顺序"));
        down.IsEnabled = position < count - 1;
        actions.Children.Add(up);
        actions.Children.Add(down);
        actions.Children.Add(Ui.Button("移除", async () => await RemoveAsync(record)));
        body.Children.Add(actions);

        // Details stay folded until asked for.
        var details = Ui.Column(6);
        details.Children.Add(Ui.Hint("目录：" + record.Directory));
        var dependencies = record.Dependencies.Where(d => d.Length > 0).ToArray();
        details.Children.Add(Ui.Hint("依赖：" + (dependencies.Length > 0 ? string.Join("、", dependencies) : "无")));
        details.Children.Add(Ui.Hint("原生库：" + (record.HasLibrary ? "有" : "无（仅页面）") + "　页面：" + (record.Ui.Length > 0 ? record.Ui : "无")));
        if (live?["entry"]?.GetValue<string>() is { Length: > 0 } entry) details.Children.Add(Ui.Hint("入口：" + entry));
        details.Children.Add(Ui.Hint("配置：" + (record.State["configuration"]?.ToJsonString() ?? "{}")));
        if (live?["logs"] is JsonArray logs && logs.Count > 0)
        {
            details.Children.Add(new TextBox
            {
                Text = string.Join("\r\n", logs.Select(l => l?.GetValue<string>() ?? "")),
                IsReadOnly = true, AcceptsReturn = true, TextWrapping = TextWrapping.Wrap, MaxHeight = 200,
                FontFamily = new Microsoft.UI.Xaml.Media.FontFamily("Cascadia Mono, Consolas"), FontSize = 12
            });
        }
        var expander = new Expander
        {
            Header = "详细信息", Content = details, HorizontalAlignment = HorizontalAlignment.Stretch,
            HorizontalContentAlignment = HorizontalAlignment.Stretch, IsExpanded = _expanded.Contains(record.Id)
        };
        expander.Expanding += (_, _) => _expanded.Add(record.Id);
        expander.Collapsed += (_, _) => _expanded.Remove(record.Id);
        body.Children.Add(expander);
        return Ui.Card(body);
    }

    private async Task ActAsync(Func<Task> action, string success)
    {
        _busy = true;
        try
        {
            await action();
            Ui.Show(_message, success, InfoBarSeverity.Success);
        }
        catch (Exception e)
        {
            Ui.Show(_message, e.Message, InfoBarSeverity.Error);
        }
        finally
        {
            _busy = false;
            Render();
        }
    }

    private async Task RemoveAsync(ModuleRecord record)
    {
        var dialog = new ContentDialog
        {
            XamlRoot = XamlRoot, Title = $"移除 {record.Name}？",
            Content = record.Dev ? "只从列表中移除，开发文件夹本身不会被删除。" : "模组会从列表中移除；如果游戏正在运行，它会在游戏重启后彻底卸载。",
            PrimaryButtonText = "移除", CloseButtonText = "取消", DefaultButton = ContentDialogButton.Close
        };
        if (await dialog.ShowAsync() != ContentDialogResult.Primary) return;
        await ActAsync(() => ModuleService.RemoveAsync(record.Id), $"已移除 {record.Name}");
    }

    private async Task PickAsync(bool folder)
    {
        if (_busy) return;
        try
        {
            string? path;
            if (folder)
            {
                var picker = new FolderPicker();
                picker.FileTypeFilter.Add("*");
                InitializeWithWindow.Initialize(picker, WindowNative.GetWindowHandle(_window));
                path = (await picker.PickSingleFolderAsync())?.Path;
            }
            else
            {
                var picker = new FileOpenPicker();
                picker.FileTypeFilter.Add(".zip");
                InitializeWithWindow.Initialize(picker, WindowNative.GetWindowHandle(_window));
                path = (await picker.PickSingleFileAsync())?.Path;
            }
            if (path is null) return;
            await ImportAsync(path);
        }
        catch (Exception e)
        {
            Ui.Show(_message, "导入失败：" + e.Message, InfoBarSeverity.Error);
        }
    }

    public async Task ImportAsync(string path)
    {
        ModuleRecord? record = null;
        await ActAsync(async () => record = await ModuleService.ImportAsync(path), "导入完成");
        if (record is not null) Ui.Show(_message, $"已安装 {record.Name} {record.Version}。" + (LaunchService.GameRunning() ? "游戏正在运行，新模组会立即尝试加载；更新已加载的模组需要重启游戏。" : ""), InfoBarSeverity.Success);
    }

    private async Task PickDevelopmentAsync()
    {
        if (_busy) return;
        try
        {
            var picker = new FolderPicker();
            picker.FileTypeFilter.Add("*");
            InitializeWithWindow.Initialize(picker, WindowNative.GetWindowHandle(_window));
            string? path = (await picker.PickSingleFolderAsync())?.Path;
            if (path is null) return;
            await ActAsync(() => ModuleService.RegisterDevelopmentFolderAsync(path), "已注册开发文件夹");
        }
        catch (Exception e)
        {
            Ui.Show(_message, "注册失败：" + e.Message, InfoBarSeverity.Error);
        }
    }
}
