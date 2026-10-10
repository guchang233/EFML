using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace EFML.Manager.Views;

// Small builders shared by the code-built pages.
internal static class Ui
{
    public static Brush Brush(string key) => (Brush)Application.Current.Resources[key];

    public static TextBlock Title(string text) => new()
    {
        Text = text, FontSize = 28, FontWeight = FontWeights.SemiBold, TextWrapping = TextWrapping.Wrap
    };

    public static TextBlock Section(string text) => new()
    {
        Text = text, FontSize = 18, FontWeight = FontWeights.SemiBold, TextWrapping = TextWrapping.Wrap,
        Margin = new Thickness(0, 12, 0, 0)
    };

    public static TextBlock Text(string text, double size = 14) => new()
    {
        Text = text, FontSize = size, TextWrapping = TextWrapping.Wrap
    };

    public static TextBlock Hint(string text) => new()
    {
        Text = text, FontSize = 12, TextWrapping = TextWrapping.Wrap,
        Foreground = Brush("TextFillColorSecondaryBrush")
    };

    public static Border Card(UIElement child, double padding = 16) => new()
    {
        Child = child,
        Padding = new Thickness(padding),
        CornerRadius = new CornerRadius(8),
        Background = Brush("CardBackgroundFillColorDefaultBrush"),
        BorderBrush = Brush("CardStrokeColorDefaultBrush"),
        BorderThickness = new Thickness(1)
    };

    public static Button Button(string text, Action click, bool accent = false)
    {
        var button = new Button { Content = text };
        if (accent) button.Style = (Style)Application.Current.Resources["AccentButtonStyle"];
        button.Click += (_, _) => click();
        return button;
    }

    public static StackPanel Row(params UIElement[] children)
    {
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        foreach (var child in children) row.Children.Add(child);
        return row;
    }

    public static StackPanel Column(double spacing, params UIElement[] children)
    {
        var column = new StackPanel { Spacing = spacing };
        foreach (var child in children) column.Children.Add(child);
        return column;
    }

    // A page: scrolling column with the standard margins.
    public static ScrollViewer Page(StackPanel content)
    {
        content.Padding = new Thickness(32, 24, 32, 32);
        content.MaxWidth = 1100;
        content.HorizontalAlignment = HorizontalAlignment.Stretch;
        return new ScrollViewer { Content = content, VerticalScrollBarVisibility = ScrollBarVisibility.Auto };
    }

    // A labelled status tile: caption, value and detail, with a coloured bar on the left.
    public sealed class Tile
    {
        private readonly Border _bar = new() { Width = 4, CornerRadius = new CornerRadius(2) };
        private readonly TextBlock _value = new() { FontSize = 18, FontWeight = FontWeights.SemiBold, TextWrapping = TextWrapping.Wrap };
        private readonly TextBlock _detail = Hint("");
        public Border Element { get; }

        public Tile(string caption)
        {
            var text = Column(2, Hint(caption), _value, _detail);
            var grid = new Grid { ColumnSpacing = 12 };
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            grid.Children.Add(_bar);
            Grid.SetColumn(text, 1);
            grid.Children.Add(text);
            Element = Card(grid, 14);
            Set("—", "", "TextFillColorTertiaryBrush");
        }

        public void Set(string value, string detail, string brush)
        {
            _value.Text = value;
            _detail.Text = detail;
            _bar.Background = Brush(brush);
        }
    }

    public static void Show(InfoBar bar, string message, InfoBarSeverity severity)
    {
        bar.Message = message;
        bar.Severity = severity;
        bar.IsOpen = message.Length > 0;
    }
}
