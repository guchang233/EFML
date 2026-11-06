using EFML.Manager.Services;
using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Windows.ApplicationModel.DataTransfer;

namespace EFML.Manager.Views;

// Crash reports in plain words: when, how bad, which module was involved and what it was doing.
// The raw stack sits in a collapsed section; "复制给 AI" copies the structured report.
internal sealed class CrashPage : UserControl
{
    private readonly StackPanel _list = new() { Spacing = 12 };
    private readonly InfoBar _message = new() { IsClosable = true };
    private readonly CheckBox _all = new() { Content = "也显示游戏可能已自行处理的异常" };
    private IReadOnlyList<CrashService.Report> _reports = [];

    public CrashPage()
    {
        _all.Checked += (_, _) => Render();
        _all.Unchecked += (_, _) => Render();
        var actions = Ui.Row(
            Ui.Button("刷新", async () => await RefreshAsync(), accent: true),
            Ui.Button("复制全部给 AI", () => Copy("[" + string.Join(",", Visible().Select(r => r.Json)) + "]", "已复制全部记录。")),
            Ui.Button("打开崩溃日志文件夹", () => Shell.OpenFolder(Path.GetDirectoryName(CrashService.LogPath)!)));
        Content = Ui.Page(Ui.Column(16,
            Ui.Title("崩溃记录"),
            Ui.Hint("游戏出现内存访问错误等异常时，EFML 会记下当时的调用栈、栈上有哪些模组，以及每个模组最后在做什么。" +
                    "“游戏崩溃”表示游戏因此退出；其余记录游戏可能已经自行处理，通常可以忽略。"),
            actions, _all, _message, _list));
        Loaded += async (_, _) => await RefreshAsync();
    }

    private async Task RefreshAsync()
    {
        var result = await CrashService.LoadAsync();
        _reports = result.Reports;
        Ui.Show(_message, result.Problem, InfoBarSeverity.Warning);
        Render();
    }

    private IEnumerable<CrashService.Report> Visible() => _all.IsChecked == true ? _reports : _reports.Where(Important);

    // A fatal fault, or any fault with an EFML module on the stack.
    private static bool Important(CrashService.Report r) => r.Fatal || r.ModulesOnStack.Count > 0;

    private void Render()
    {
        _list.Children.Clear();
        var visible = Visible().ToList();
        if (visible.Count == 0)
        {
            int hidden = _reports.Count;
            _list.Children.Add(Ui.Card(Ui.Text(hidden > 0
                ? $"没有需要关注的记录（另有 {hidden} 条游戏可能已自行处理的异常，勾选上方选项可查看）。"
                : "还没有崩溃记录。")));
            return;
        }
        foreach (var report in visible) _list.Children.Add(Card(report));
    }

    private static string Describe(string exception) => exception switch
    {
        "access violation" => "内存访问错误",
        "illegal instruction" => "非法指令",
        "stack overflow" => "栈溢出",
        "integer divide by zero" => "整数除以零",
        "privileged instruction" => "特权指令",
        "heap corruption" => "堆损坏",
        "stack buffer overrun" => "栈缓冲区溢出",
        _ => exception
    };

    private static string Access(string access)
    {
        if (access.Length == 0) return "";
        var parts = access.Split(' ', 2);
        string kind = parts[0] switch { "read" => "读取", "write" => "写入", "execute" => "执行", _ => parts[0] };
        return $"（{kind} {(parts.Length > 1 ? parts[1] : "")}）";
    }

    private static string Where(CrashService.Frame frame)
    {
        string text = frame.Address;
        if (frame.Method.Length > 0) text += "  " + frame.Method;
        if (frame.Module.Length > 0) text += $"  [模组 {frame.Module}]";
        return text;
    }

    private Border Card(CrashService.Report r)
    {
        var badge = new Border
        {
            CornerRadius = new CornerRadius(4), Padding = new Thickness(8, 2, 8, 2),
            Background = Ui.Brush(r.Fatal ? "SystemFillColorCriticalBackgroundBrush" : "SystemFillColorNeutralBackgroundBrush"),
            Child = new TextBlock
            {
                Text = r.Fatal ? "游戏崩溃" : "已记录的异常", FontSize = 12,
                Foreground = Ui.Brush(r.Fatal ? "SystemFillColorCriticalBrush" : "TextFillColorSecondaryBrush")
            }
        };
        var header = Ui.Row(new TextBlock { Text = r.Time, FontWeight = FontWeights.SemiBold, VerticalAlignment = VerticalAlignment.Center }, badge);

        var body = Ui.Column(6, header,
            Ui.Text($"{Describe(r.Exception)}{Access(r.Access)}，位置：{Where(r.Location)}"));
        if (r.ModulesOnStack.Count > 0)
            body.Children.Add(Ui.Text("出错时栈上的模组：" + string.Join("、", r.ModulesOnStack)));
        else
            body.Children.Add(Ui.Hint("出错时栈上没有 EFML 模组。"));
        foreach (var step in r.Steps)
            body.Children.Add(Ui.Hint($"{step.Module} 当时在：{step.Text}（{step.MsAgo} 毫秒前）"));

        var stack = new TextBlock
        {
            FontFamily = new FontFamily("Consolas"), FontSize = 12, IsTextSelectionEnabled = true,
            TextWrapping = TextWrapping.NoWrap,
            Text = string.Join("\n", r.Stack.Select(f => $"#{f.Index:00}  {Where(f)}"))
        };
        body.Children.Add(new Expander
        {
            Header = $"调用栈（{r.Stack.Count} 帧）", HorizontalAlignment = HorizontalAlignment.Stretch,
            HorizontalContentAlignment = HorizontalAlignment.Stretch,
            Content = new ScrollViewer { Content = stack, HorizontalScrollBarVisibility = ScrollBarVisibility.Auto }
        });
        body.Children.Add(Ui.Row(Ui.Button("复制给 AI", () => Copy(r.Json, "已复制这条记录。"))));
        return Ui.Card(body);
    }

    private void Copy(string text, string done)
    {
        var package = new DataPackage();
        package.SetText(text);
        Clipboard.SetContent(package);
        Ui.Show(_message, done, InfoBarSeverity.Success);
    }
}
