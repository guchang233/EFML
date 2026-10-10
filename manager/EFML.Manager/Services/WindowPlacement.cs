using System.Runtime.InteropServices;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Windows.Graphics;
using WinRT.Interop;

namespace EFML.Manager.Services;

// Derived from Better-Endfield's WindowPlacementService (AGPL-3.0).
internal static class WindowPlacement
{
    public static void SetInitialSize(Window window, int widthDip, int heightDip, Window? owner = null)
    {
        try
        {
            Window displayWindow = owner ?? window;
            DisplayArea? display = DisplayArea.GetFromWindowId(displayWindow.AppWindow.Id, DisplayAreaFallback.Nearest);
            if (display is null) return;
            RectInt32 area = display.WorkArea;
            if (owner is not null) window.AppWindow.Move(new PointInt32(area.X, area.Y));
            double scale = Scale(window);
            int inset = (int)Math.Round(16 * scale);
            int width = (int)Math.Clamp(Math.Round(widthDip * scale), 1, Math.Max(1, area.Width - inset * 2));
            int height = (int)Math.Clamp(Math.Round(heightDip * scale), 1, Math.Max(1, area.Height - inset * 2));
            window.AppWindow.MoveAndResize(new RectInt32(area.X + (area.Width - width) / 2,
                area.Y + (area.Height - height) / 2, width, height));
        }
        catch (InvalidOperationException)
        {
        }
    }

    public static void SetMinimumSize(Window window, int widthDip, int heightDip)
    {
        if (window.AppWindow.Presenter is OverlappedPresenter presenter)
        {
            double scale = Scale(window);
            presenter.PreferredMinimumWidth = (int)Math.Round(widthDip * scale);
            presenter.PreferredMinimumHeight = (int)Math.Round(heightDip * scale);
        }
    }

    public static void SetIcon(Window window)
    {
        string icon = Path.Combine(AppContext.BaseDirectory, "Assets", "efml.ico");
        if (File.Exists(icon)) window.AppWindow.SetIcon(icon);
    }

    private static double Scale(Window window)
    {
        uint dpi = GetDpiForWindow(WindowNative.GetWindowHandle(window));
        return (dpi == 0 ? 96 : dpi) / 96.0;
    }

    [DllImport("user32.dll")]
    [DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
    private static extern uint GetDpiForWindow(nint hwnd);
}
