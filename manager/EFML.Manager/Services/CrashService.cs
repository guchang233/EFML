using System.Diagnostics;
using System.Text.Json;

namespace EFML.Manager.Services;

// Crash reports come from `efml crash --json`, the same parser scripts and AI tools use, so the
// manager never has its own reading of crash.log.
internal static class CrashService
{
    public sealed record Frame(int Index, string Address, string Method, string Module);
    public sealed record Step(string Module, string Text, long MsAgo);
    public sealed record Report(string Time, bool Fatal, string Exception, string Code, string Access,
        Frame Location, IReadOnlyList<string> ModulesOnStack, IReadOnlyList<Step> Steps,
        IReadOnlyList<Frame> Stack, string Json);
    public sealed record Result(IReadOnlyList<Report> Reports, string Problem);

    public static string LogPath => Path.Combine(AppPaths.DataRoot, "crash", "crash.log");

    private static string? Cli()
    {
        foreach (string folder in new[] { AppPaths.InstalledRuntime, AppPaths.BundledRuntime })
        {
            string path = Path.Combine(folder, "efml.exe");
            if (File.Exists(path)) return path;
        }
        return null;
    }

    public static async Task<Result> LoadAsync()
    {
        if (!File.Exists(LogPath)) return new Result([], "");
        string? cli = Cli();
        if (cli is null) return new Result([], "找不到 efml.exe，请在“设置”中重新安装运行时。");
        var start = new ProcessStartInfo(cli, "crash --json --all --last 50")
        {
            UseShellExecute = false, CreateNoWindow = true,
            RedirectStandardOutput = true, RedirectStandardError = true,
            StandardOutputEncoding = System.Text.Encoding.UTF8
        };
        start.Environment["EFML_HOME"] = AppPaths.DataRoot;
        using var process = Process.Start(start);
        if (process is null) return new Result([], "无法运行 efml.exe。");
        string output = await process.StandardOutput.ReadToEndAsync();
        await process.WaitForExitAsync();
        if (process.ExitCode != 0)
            return new Result([], "运行时版本较旧，不支持读取崩溃记录。请在“设置”中重新安装运行时。");
        try
        {
            using var document = JsonDocument.Parse(output);
            var reports = new List<Report>();
            foreach (var item in document.RootElement.EnumerateArray())
            {
                if (item.TryGetProperty("process_start", out _)) continue;
                reports.Add(Read(item));
            }
            reports.Reverse(); // newest first
            return new Result(reports, "");
        }
        catch (JsonException)
        {
            return new Result([], "崩溃记录格式无法识别。");
        }
    }

    private static string Text(JsonElement e, string name) =>
        e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() ?? "" : "";

    private static Frame ReadFrame(JsonElement e) => new(
        e.TryGetProperty("index", out var i) && i.ValueKind == JsonValueKind.Number ? i.GetInt32() : 0,
        Text(e, "address"), Text(e, "method"), Text(e, "module"));

    private static Report Read(JsonElement e)
    {
        string access = "";
        if (e.TryGetProperty("access", out var a)) access = $"{Text(a, "kind")} {Text(a, "address")}";
        var modules = e.TryGetProperty("modules_on_stack", out var m)
            ? m.EnumerateArray().Select(x => x.GetString() ?? "").ToList() : [];
        var steps = e.TryGetProperty("steps", out var s)
            ? s.EnumerateArray().Select(x => new Step(Text(x, "module"), Text(x, "step"),
                x.TryGetProperty("ms_ago", out var t) && t.ValueKind == JsonValueKind.Number ? t.GetInt64() : 0)).ToList()
            : [];
        var stack = e.TryGetProperty("stack", out var k) ? k.EnumerateArray().Select(ReadFrame).ToList() : [];
        var location = e.TryGetProperty("location", out var l) ? ReadFrame(l) : new Frame(0, "", "", "");
        return new Report(Text(e, "time"), e.TryGetProperty("fatal", out var f) && f.ValueKind == JsonValueKind.True,
            Text(e, "exception"), Text(e, "code"), access, location, modules, steps, stack,
            e.GetRawText());
    }
}
