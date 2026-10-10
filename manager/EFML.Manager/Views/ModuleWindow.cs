using EFML.Manager.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.Web.WebView2.Core;
using System.Text.Json.Nodes;

namespace EFML.Manager.Views;

// A module's own page in WebView2. Derived from Better-Endfield's ThirdPartyModuleWindow (AGPL-3.0):
// each installed generation is its own https origin mapped to the package folder, navigation is
// confined to it, and requests from the page are relayed to the Host's RPC for this module only.
internal sealed class ModuleWindow : Window
{
    private static readonly Dictionary<string, ModuleWindow> Windows = new(StringComparer.Ordinal);
    private readonly ModuleRecord _record;
    private readonly WebView2 _view = new();
    private readonly TextBlock _status = new() { Margin = new Thickness(12, 8, 12, 8), TextWrapping = TextWrapping.Wrap, FontSize = 12 };
    private readonly DispatcherTimer _poll = new() { Interval = TimeSpan.FromMilliseconds(500) };
    private readonly string _host;
    private bool _closed, _polling;

    private ModuleWindow(ModuleRecord record)
    {
        _record = record;
        string origin = (record.Dev ? record.Generation[4..] : record.Generation).Replace("-", "");
        _host = "m-" + origin + ".efml.local";
        Title = "EFML · " + record.Name;
        _status.Text = "正在打开模块页面…";
        var grid = new Grid();
        grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        grid.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        grid.Children.Add(_status);
        Grid.SetRow(_view, 1);
        grid.Children.Add(_view);
        Content = grid;
        WindowPlacement.SetInitialSize(this, 1120, 820, App.MainWindowInstance);
        WindowPlacement.SetIcon(this);
        _view.Loaded += Initialize;
        Closed += (_, _) =>
        {
            _closed = true;
            _poll.Stop();
            _view.Close();
            if (Windows.GetValueOrDefault(record.Id) == this) Windows.Remove(record.Id);
        };
        _poll.Tick += Poll;
    }

    public static void Open(ModuleRecord record)
    {
        if (record.Ui.Length == 0) throw new InvalidOperationException("该模块没有页面");
        if (Windows.TryGetValue(record.Id, out var existing))
        {
            if (existing._record.Generation == record.Generation)
            {
                existing.Activate();
                return;
            }
            existing.Close();
        }
        var window = new ModuleWindow(record);
        Windows[record.Id] = window;
        window.Activate();
    }

    public static void CloseAll()
    {
        foreach (var window in Windows.Values.ToArray()) window.Close();
    }

    private bool Owned(string uri) =>
        Uri.TryCreate(uri, UriKind.Absolute, out var value) && value.Scheme == "https" && value.Host == _host;

    private async void Initialize(object sender, RoutedEventArgs args)
    {
        _view.Loaded -= Initialize;
        try
        {
            var environment = await CoreWebView2Environment.CreateWithOptionsAsync(null,
                Path.Combine(AppPaths.DataRoot, "webview-profile"), null);
            if (_closed) return;
            await _view.EnsureCoreWebView2Async(environment);
            if (_closed) return;
            var core = _view.CoreWebView2;
            core.Settings.AreHostObjectsAllowed = false;
            core.Settings.AreDevToolsEnabled = _record.Dev;
            core.SetVirtualHostNameToFolderMapping(_host, _record.Directory, CoreWebView2HostResourceAccessKind.DenyCors);
            core.NavigationStarting += (_, e) => { if (!Owned(e.Uri)) e.Cancel = true; };
            core.NewWindowRequested += (_, e) => e.Handled = true;
            core.WebMessageReceived += Message;
            await core.AddScriptToExecuteOnDocumentCreatedAsync(WebBridge.Script);
            core.NavigationCompleted += (_, e) =>
            {
                if (!_closed) _status.Text = e.IsSuccess ? $"{_record.Name} {_record.Version} · {_record.Id}" : "页面加载失败";
            };
            _view.Source = new Uri("https://" + _host + "/" + _record.Ui);
            _poll.Start();
        }
        catch (Exception e)
        {
            if (!_closed) _status.Text = "无法打开页面（需要 WebView2 运行时）：" + e.Message;
        }
    }

    private void Reply(string requestId, JsonNode? value, string? error = null)
    {
        if (_closed) return;
        _view.CoreWebView2.PostWebMessageAsJson(new JsonObject
        {
            ["kind"] = "bridge_reply",
            ["request_id"] = requestId,
            ["value"] = value?.DeepClone(),
            ["error"] = error
        }.ToJsonString());
    }

    private async void Message(object? sender, CoreWebView2WebMessageReceivedEventArgs e)
    {
        if (_closed || !Owned(e.Source) || e.WebMessageAsJson.Length > 256 * 1024) return;
        string requestId = "";
        try
        {
            var message = JsonNode.Parse(e.WebMessageAsJson)!.AsObject();
            if (!WebBridge.AcceptedProtocols.Contains(message["protocol"]?.GetValue<string>() ?? "")) return;
            requestId = message["request_id"]!.GetValue<string>();
            if (requestId.Length > 128) return;
            switch (message["operation"]!.GetValue<string>())
            {
                case "readConfig":
                    Reply(requestId, ModuleService.Record(_record.Id).State["configuration"]);
                    break;
                case "saveConfig":
                    var configuration = message["payload"] as JsonObject ?? throw new InvalidDataException("配置必须是对象");
                    await ModuleService.SaveConfigurationAsync(_record.Id, configuration);
                    try
                    {
                        await ModuleService.RuntimeAsync("configure", _record.Id, configuration);
                    }
                    catch (Exception)
                    {
                        // Saved for the next game start when the game is not running.
                    }
                    Reply(requestId, new JsonObject { ["saved"] = true });
                    break;
                case "send":
                    Reply(requestId, await ModuleService.RuntimeAsync("send", _record.Id, message["payload"], requestId));
                    break;
                case "status":
                    try
                    {
                        Reply(requestId, await ModuleService.RuntimeAsync("status", _record.Id));
                    }
                    catch (Exception)
                    {
                        Reply(requestId, new JsonObject { ["connected"] = false });
                    }
                    break;
                default:
                    throw new InvalidOperationException("不支持的页面请求");
            }
        }
        catch (Exception error)
        {
            if (requestId.Length > 0) Reply(requestId, null, error.Message);
        }
    }

    private async void Poll(object? sender, object e)
    {
        if (_closed || _polling || _view.CoreWebView2 is null) return;
        _polling = true;
        try
        {
            var result = await ModuleService.RuntimeAsync("poll", _record.Id);
            if (!_closed && result["messages"] is JsonArray messages)
            {
                foreach (var message in messages)
                {
                    _view.CoreWebView2.PostWebMessageAsJson(new JsonObject
                    {
                        ["kind"] = "runtime_message",
                        ["message"] = message?.DeepClone()
                    }.ToJsonString());
                }
            }
        }
        catch (Exception)
        {
            // The game may be closed while the page stays open.
        }
        finally
        {
            _polling = false;
        }
    }
}
