using EFML.Manager.Services;
using EFML.Manager.Views;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;

namespace EFML.Manager;

public sealed class MainWindow : Window
{
    internal ManagerSettings Settings { get; } = ManagerSettings.Load();
    private readonly Grid _root = new();
    private readonly NavigationView _navigation = new();
    private readonly Grid _host = new();
    private readonly Dictionary<string, UIElement> _pages = new();

    public MainWindow()
    {
        Title = "EFML";
        SystemBackdrop = new MicaBackdrop();
        WindowPlacement.SetIcon(this);
        WindowPlacement.SetInitialSize(this, 1180, 800);
        WindowPlacement.SetMinimumSize(this, 900, 640);

        // Fixed Left mode: collapsing leaves the icon strip and the content always reflows beside
        // the pane (Auto mode would switch between inline and overlay panes with the width).
        _navigation.PaneDisplayMode = NavigationViewPaneDisplayMode.Left;
        _navigation.IsBackButtonVisible = NavigationViewBackButtonVisible.Collapsed;
        _navigation.IsSettingsVisible = true;
        _navigation.OpenPaneLength = 220;
        _navigation.CompactPaneLength = 48;
        _navigation.IsPaneOpen = Settings.PaneOpen;
        _navigation.PaneOpened += (_, _) => RememberPane(true);
        _navigation.PaneClosed += (_, _) => RememberPane(false);
        _navigation.PaneHeader = PaneHeader();
        _navigation.Content = _host;
        _navigation.MenuItems.Add(Item("启动", "home", ""));
        _navigation.MenuItems.Add(Item("模组", "modules", ""));
        _navigation.FooterMenuItems.Add(Item("关于", "about", ""));
        _navigation.SelectionChanged += (_, e) =>
        {
            string tag = e.IsSettingsSelected ? "settings" : (e.SelectedItem as NavigationViewItem)?.Tag as string ?? "home";
            Navigate(tag);
        };
        _navigation.SelectedItem = _navigation.MenuItems[0];
        _root.Children.Add(_navigation);
        Content = _root;
        ApplyTheme();
        _root.Loaded += async (_, _) => await FirstRunAsync();
        Closed += (_, _) => ModuleWindow.CloseAll();
    }

    private static NavigationViewItem Item(string text, string tag, string glyph) => new()
    {
        Content = text, Tag = tag, Icon = new FontIcon { Glyph = glyph }
    };

    private static UIElement PaneHeader()
    {
        var grid = new Grid { Margin = new Thickness(4, 8, 8, 12), ColumnSpacing = 10 };
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(34) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        string png = Path.Combine(AppContext.BaseDirectory, "Assets", "efml.png");
        if (File.Exists(png))
        {
            grid.Children.Add(new Border
            {
                Width = 34, Height = 34, CornerRadius = new CornerRadius(6),
                Child = new Image { Source = new BitmapImage(new Uri(png)), Stretch = Stretch.UniformToFill }
            });
        }
        var text = Ui.Column(1,
            new TextBlock { Text = "EFML", FontSize = 16, FontWeight = Microsoft.UI.Text.FontWeights.SemiBold },
            new TextBlock { Text = "终末地模组加载器", FontSize = 11, Foreground = Ui.Brush("TextFillColorSecondaryBrush") });
        text.VerticalAlignment = VerticalAlignment.Center;
        Grid.SetColumn(text, 1);
        grid.Children.Add(text);
        return grid;
    }

    private void Navigate(string tag)
    {
        if (!_pages.TryGetValue(tag, out var page))
        {
            page = tag switch
            {
                "modules" => new ModulesPage(this),
                "settings" => new SettingsPage(this),
                "about" => new AboutPage(),
                _ => new HomePage(this)
            };
            _pages[tag] = page;
        }
        // The navigation view keeps one content host; pages are swapped inside it.
        _host.Children.Clear();
        _host.Children.Add(page);
    }

    private void RememberPane(bool open)
    {
        if (Settings.PaneOpen == open) return;
        Settings.PaneOpen = open;
        try
        {
            Settings.Save();
        }
        catch (Exception exception)
        {
            App.CrashLog("SaveSettings", exception);
        }
    }

    internal void ApplyTheme()
    {
        _root.RequestedTheme = Settings.Theme switch
        {
            "light" => ElementTheme.Light,
            "dark" => ElementTheme.Dark,
            _ => ElementTheme.Default
        };
    }

    // First start: find the game and install the runtime.
    private async Task FirstRunAsync()
    {
        try
        {
            if (!GameDiscovery.IsGameExecutable(Settings.GamePath))
            {
                string found = await Task.Run(() => GameDiscovery.Discover(Settings.GamePath));
                if (found.Length > 0) Settings.GamePath = found;
            }
            await RuntimeService.InstallAsync();
        }
        catch (Exception exception)
        {
            App.CrashLog("FirstRun", exception);
        }
        if (!Settings.FirstRunDone)
        {
            var dialog = new ContentDialog
            {
                XamlRoot = _root.XamlRoot,
                Title = "欢迎使用 EFML",
                Content = Ui.Column(12,
                    Ui.Text(GameDiscovery.IsGameExecutable(Settings.GamePath) ? "已找到游戏：" + Settings.GamePath : "没有自动找到游戏，稍后可以在「设置」中选择 Endfield.exe。"),
                    Ui.Text("EFML 不直接启动游戏：先在「模组」里安装并启用模组，再到「启动」页点击「启动游戏」，EFML 会准备好加载组件，然后请用鹰角官方启动器开始游戏，模组会自动加载。")),
                PrimaryButtonText = "开始使用",
                DefaultButton = ContentDialogButton.Primary
            };
            await dialog.ShowAsync();
            Settings.FirstRunDone = true;
        }
        try
        {
            Settings.Save();
        }
        catch (Exception exception)
        {
            App.CrashLog("SaveSettings", exception);
        }
        if (_host.Children.FirstOrDefault() is HomePage home) await home.RefreshAsync();
    }
}
