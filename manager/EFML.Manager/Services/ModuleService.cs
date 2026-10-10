using System.IO.Compression;
using System.Net;
using System.Net.Http.Headers;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;

namespace EFML.Manager.Services;

internal sealed record ModuleRecord(JsonObject State, JsonObject Manifest, string Error = "")
{
    public string Id => State["id"]!.GetValue<string>();
    public string Generation => State["generation"]?.GetValue<string>() ?? "";
    public string Directory => State["directory"]!.GetValue<string>();
    public bool Dev => Generation.StartsWith("dev-", StringComparison.Ordinal);
    public string Name => Manifest["name"]?.GetValue<string>() ?? Id;
    public string Author => Manifest["author"]?.GetValue<string>() ?? "";
    public string Version => Manifest["version"]?.GetValue<string>() ?? "";
    public string Description => Manifest["description"]?.GetValue<string>() ?? "";
    public bool Enabled => State["enabled"]?.GetValue<bool>() ?? false;
    public string Ui => Manifest["ui"]?.GetValue<string>() ?? "";
    public bool HasLibrary => Manifest["libraries"]?["windows-x64"] is not null;
    public bool Supported => Error.Length == 0 && (Ui.Length > 0 || HasLibrary);
    public IEnumerable<string> Dependencies =>
        Manifest["dependencies"] is JsonArray list ? list.Select(d => d?.GetValue<string>() ?? "") : [];
}

// Installed modules in modules\index.json. Derived from Better-Endfield's ThirdPartyModuleService
// (AGPL-3.0): same index, package checks and runtime RPC. EFML adds folder import and in-place
// development modules (generation "dev-...").
internal sealed class ModuleService
{
    internal const long ArchiveLimit = 256L * 1024 * 1024, EntryLimit = 128L * 1024 * 1024;
    private static readonly SemaphoreSlim Gate = new(1, 1);
    private static readonly HttpClient Http = new(new HttpClientHandler { UseProxy = false }) { Timeout = TimeSpan.FromSeconds(2) };

    public static string Root => AppPaths.ModulesRoot;
    public static string IndexPath => AppPaths.IndexPath;

    internal static void RequireId(string id)
    {
        if (!Regex.IsMatch(id, @"\A[A-Za-z0-9][A-Za-z0-9_.-]{0,95}\z") ||
            id.StartsWith("efml.", StringComparison.OrdinalIgnoreCase) ||
            id.StartsWith("betterendfield.", StringComparison.OrdinalIgnoreCase) ||
            id.StartsWith("betterendfieldnext.", StringComparison.OrdinalIgnoreCase) ||
            id.Equals("voice.character", StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidDataException($"模块 ID 无效或为保留名：{id}");
        }
    }

    internal static string Relative(string value)
    {
        if (value.Length == 0 || value.Length > 240 || value.Contains('\\') || value.Contains(':') || value.StartsWith('/'))
            throw new InvalidDataException($"包内路径不安全：{value}");
        foreach (string part in value.Split('/'))
        {
            if (part.Length == 0 || part is "." or ".." || part.EndsWith('.') || part.EndsWith(' ') || part.Any(char.IsControl) ||
                Regex.IsMatch(part.Split('.')[0], @"\A(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])\z", RegexOptions.IgnoreCase))
                throw new InvalidDataException($"包内路径不安全：{value}");
        }
        return value;
    }

    internal static JsonObject ReadManifest(string directory)
    {
        string path = Path.Combine(directory, "module.json");
        if (!File.Exists(path)) throw new InvalidDataException("模块根目录没有 module.json");
        if (new FileInfo(path).Length > 256 * 1024) throw new InvalidDataException("module.json 超过 256 KiB");
        var manifest = JsonNode.Parse(File.ReadAllText(path, Encoding.UTF8))?.AsObject()
            ?? throw new InvalidDataException("module.json 不是 JSON 对象");
        if (manifest["format"]?.GetValue<int>() != 1 || manifest["abi"]?.GetValue<int>() != 1)
            throw new InvalidDataException("module.json 需要 format 1 与 abi 1");
        RequireId(manifest["id"]?.GetValue<string>() ?? "");
        if (manifest["name"]?.GetValue<string>() is not string name || name.Length == 0 || name.Length > 200)
            throw new InvalidDataException("module.json 缺少 name");
        foreach (string key in new[] { "author", "version", "description" })
        {
            if (manifest[key] is not null && (manifest[key]!.GetValue<string>() is not string value || value.Length > 2000))
                throw new InvalidDataException($"module.json 的 {key} 无效");
        }
        var libraries = manifest["libraries"] as JsonObject ?? new JsonObject();
        foreach (var library in libraries)
        {
            string relative = Relative(library.Value!.GetValue<string>());
            if (!File.Exists(Path.Combine(directory, relative)) ||
                (library.Key == "windows-x64" && !relative.EndsWith(".dll", StringComparison.OrdinalIgnoreCase)))
                throw new InvalidDataException($"模块库文件缺失或类型不符：{relative}");
        }
        string ui = manifest["ui"]?.GetValue<string>() ?? "";
        if (ui.Length > 0 && (!Relative(ui).EndsWith(".html", StringComparison.OrdinalIgnoreCase) || !File.Exists(Path.Combine(directory, ui))))
            throw new InvalidDataException($"模块页面缺失：{ui}");
        if (ui.Length == 0 && libraries.Count == 0) throw new InvalidDataException("模块既没有原生库也没有页面");
        if (manifest["default_configuration"] is not null and not JsonObject)
            throw new InvalidDataException("default_configuration 必须是对象");
        if (manifest["dependencies"] is JsonArray dependencies)
        {
            if (dependencies.Count > 128) throw new InvalidDataException("依赖过多");
            foreach (var dependency in dependencies)
            {
                string dependencyId = dependency!.GetValue<string>();
                RequireId(dependencyId);
                if (dependencyId == manifest["id"]!.GetValue<string>()) throw new InvalidDataException("模块不能依赖自己");
            }
        }
        else if (manifest["dependencies"] is not null)
        {
            throw new InvalidDataException("dependencies 必须是数组");
        }
        return manifest;
    }

    internal static JsonObject NewIndex()
    {
        var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        int port = ((IPEndPoint)listener.LocalEndpoint).Port;
        listener.Stop();
        return new JsonObject
        {
            ["schema"] = 1,
            ["port"] = port,
            ["token"] = Convert.ToHexString(RandomNumberGenerator.GetBytes(32)).ToLowerInvariant(),
            ["modules"] = new JsonArray()
        };
    }

    public static JsonObject LoadIndex()
    {
        if (!File.Exists(IndexPath)) return NewIndex();
        if (new FileInfo(IndexPath).Length > 1024 * 1024) throw new InvalidDataException("index.json 超过 1 MiB");
        var index = JsonNode.Parse(File.ReadAllText(IndexPath, Encoding.UTF8))!.AsObject();
        if (index["schema"]?.GetValue<int>() != 1 || index["port"]?.GetValue<int>() is not int port || port < 1024 || port > 65535 ||
            index["token"]?.GetValue<string>() is not string token || !Regex.IsMatch(token, @"\A[A-Za-z0-9_-]{32,128}\z") ||
            index["modules"] is not JsonArray modules)
            throw new InvalidDataException("index.json 格式无效");
        var ids = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var node in modules)
        {
            var state = node!.AsObject();
            string id = state["id"]!.GetValue<string>();
            RequireId(id);
            if (!ids.Add(id) || state["configuration"] is not JsonObject || state["directory"] is null)
                throw new InvalidDataException($"index.json 中的模块记录无效：{id}");
        }
        return index;
    }

    public static async Task EnsureIndexAsync()
    {
        if (File.Exists(IndexPath)) return;
        await Gate.WaitAsync();
        try
        {
            if (!File.Exists(IndexPath)) await WriteAsync(NewIndex());
        }
        finally
        {
            Gate.Release();
        }
    }

    private static async Task WriteAsync(JsonObject index)
    {
        System.IO.Directory.CreateDirectory(Root);
        string value = index.ToJsonString(new System.Text.Json.JsonSerializerOptions { WriteIndented = true });
        if (Encoding.UTF8.GetByteCount(value) > 1024 * 1024) throw new InvalidDataException("配置过大");
        string temporary = Path.Combine(Root, Guid.NewGuid() + ".tmp");
        try
        {
            await File.WriteAllTextAsync(temporary, value, new UTF8Encoding(false));
            File.Move(temporary, IndexPath, true);
        }
        finally
        {
            if (File.Exists(temporary)) File.Delete(temporary);
        }
    }

    public static IReadOnlyList<ModuleRecord> Records()
    {
        var records = new List<ModuleRecord>();
        foreach (var node in LoadIndex()["modules"]!.AsArray())
        {
            var state = node!.AsObject();
            try
            {
                records.Add(new ModuleRecord(state, ReadManifest(state["directory"]!.GetValue<string>())));
            }
            catch (Exception error) when (error is IOException or InvalidDataException or InvalidOperationException or
                                              ArgumentException or NullReferenceException or System.Text.Json.JsonException)
            {
                records.Add(new ModuleRecord(state, new JsonObject { ["name"] = state["id"]!.GetValue<string>() }, error.Message));
            }
        }
        return records;
    }

    public static ModuleRecord Record(string id) =>
        Records().FirstOrDefault(record => record.Id == id) ?? throw new InvalidOperationException("模块已被移除");

    private static async Task ChangeAsync(Action<JsonObject, JsonArray> change)
    {
        await Gate.WaitAsync();
        try
        {
            var index = LoadIndex();
            change(index, index["modules"]!.AsArray());
            await WriteAsync(index);
        }
        finally
        {
            Gate.Release();
        }
    }

    private static JsonObject Find(JsonArray modules, string id) =>
        modules.FirstOrDefault(m => m!["id"]!.GetValue<string>() == id)?.AsObject()
        ?? throw new InvalidOperationException("模块已被移除");

    public static Task SetEnabledAsync(string id, bool enabled) => ChangeAsync((_, modules) =>
    {
        var state = Find(modules, id);
        if (enabled && !new ModuleRecord(state, ReadManifest(state["directory"]!.GetValue<string>())).Supported)
            throw new InvalidOperationException("该模块没有 Windows 原生库或页面，无法启用");
        state["enabled"] = enabled;
    });

    public static Task SaveConfigurationAsync(string id, JsonObject configuration) =>
        ChangeAsync((_, modules) => Find(modules, id)["configuration"] = configuration.DeepClone());

    public static Task MoveAsync(string id, int direction) => ChangeAsync((_, modules) =>
    {
        int from = modules.Select(m => m!["id"]!.GetValue<string>()).ToList().IndexOf(id), to = from + direction;
        if (from < 0 || to < 0 || to >= modules.Count) return;
        var record = modules[from];
        modules.RemoveAt(from);
        modules.Insert(to, record);
    });

    // Package folders are retired, never deleted: the game may still have their DLLs loaded.
    public static Task RemoveAsync(string id) => ChangeAsync((index, modules) =>
    {
        var record = Find(modules, id);
        if (!(record["generation"]?.GetValue<string>() ?? "").StartsWith("dev-", StringComparison.Ordinal))
            (index["retired_directories"] ??= new JsonArray()).AsArray().Add(record["directory"]!.GetValue<string>());
        modules.Remove(record);
    });

    private static async Task<ModuleRecord> PublishAsync(string directory, string generation)
    {
        var manifest = ReadManifest(directory);
        string id = manifest["id"]!.GetValue<string>();
        JsonObject? installed = null;
        await ChangeAsync((index, modules) =>
        {
            var previous = modules.FirstOrDefault(m => m!["id"]!.GetValue<string>().Equals(id, StringComparison.OrdinalIgnoreCase));
            if (previous is not null && previous["id"]!.GetValue<string>() != id)
                throw new InvalidDataException("已有仅大小写不同的同名模块");
            installed = new JsonObject
            {
                ["id"] = id,
                ["enabled"] = previous?["enabled"]?.GetValue<bool>() ?? true,
                ["directory"] = directory,
                ["generation"] = generation,
                ["configuration"] = previous?["configuration"]?.DeepClone() ?? manifest["default_configuration"]?.DeepClone() ?? new JsonObject()
            };
            if (!new ModuleRecord(installed, manifest).Supported) installed["enabled"] = false;
            if (previous is not null)
            {
                int position = modules.IndexOf(previous);
                if (!(previous["generation"]?.GetValue<string>() ?? "").StartsWith("dev-", StringComparison.Ordinal))
                    (index["retired_directories"] ??= new JsonArray()).AsArray().Add(previous["directory"]!.GetValue<string>());
                modules.RemoveAt(position);
                modules.Insert(position, installed);
            }
            else
            {
                modules.Add(installed);
            }
        });
        return new ModuleRecord(installed!, manifest);
    }

    public static async Task<ModuleRecord> ImportAsync(string source)
    {
        if (System.IO.Directory.Exists(source)) return await ImportFolderAsync(source);
        if (new FileInfo(source).Length > ArchiveLimit) throw new InvalidDataException("压缩包超过 256 MiB");
        string generation = Guid.NewGuid().ToString(), directory = Path.Combine(Root, "packages", generation);
        System.IO.Directory.CreateDirectory(directory);
        bool published = false;
        try
        {
            using (var zip = ZipFile.OpenRead(source))
            {
                if (zip.Entries.Count > 4096) throw new InvalidDataException("压缩包内文件过多");
                var names = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                long declared = 0, actual = 0;
                foreach (var entry in zip.Entries)
                {
                    bool folder = entry.FullName.EndsWith('/');
                    string relative = Relative(folder ? entry.FullName[..^1] : entry.FullName);
                    if (!names.Add(relative) || ((entry.ExternalAttributes >> 16) & 0xF000) == 0xA000 || entry.Length > EntryLimit ||
                        (declared += entry.Length) > ArchiveLimit) throw new InvalidDataException("压缩包内容无效");
                    string destination = Path.Combine(directory, relative);
                    if (folder)
                    {
                        System.IO.Directory.CreateDirectory(destination);
                        continue;
                    }
                    System.IO.Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
                    await using var input = entry.Open();
                    await using var output = File.Create(destination);
                    byte[] buffer = new byte[65536];
                    long copied = 0;
                    int count;
                    while ((count = await input.ReadAsync(buffer)) > 0)
                    {
                        copied += count;
                        actual += count;
                        if (copied > EntryLimit || actual > ArchiveLimit) throw new InvalidDataException("解压后体积超限");
                        await output.WriteAsync(buffer.AsMemory(0, count));
                    }
                    if (copied != entry.Length) throw new InvalidDataException("压缩包条目长度不一致");
                }
                if (!zip.Entries.Any(e => e.FullName == "module.json")) throw new InvalidDataException("module.json 必须位于压缩包根目录");
            }
            var record = await PublishAsync(directory, generation);
            published = true;
            return record;
        }
        finally
        {
            if (!published && System.IO.Directory.Exists(directory)) System.IO.Directory.Delete(directory, true);
        }
    }

    private static async Task<ModuleRecord> ImportFolderAsync(string source)
    {
        ReadManifest(source);
        string generation = Guid.NewGuid().ToString(), directory = Path.Combine(Root, "packages", generation);
        bool published = false;
        try
        {
            foreach (string file in System.IO.Directory.EnumerateFiles(source, "*", SearchOption.AllDirectories))
            {
                string relative = Path.GetRelativePath(source, file);
                string destination = Path.Combine(directory, relative);
                System.IO.Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
                File.Copy(file, destination);
            }
            var record = await PublishAsync(directory, generation);
            published = true;
            return record;
        }
        finally
        {
            if (!published && System.IO.Directory.Exists(directory)) System.IO.Directory.Delete(directory, true);
        }
    }

    // Development: the folder stays where it is; rebuilt DLLs load after a game restart.
    public static Task<ModuleRecord> RegisterDevelopmentFolderAsync(string folder) =>
        PublishAsync(Path.GetFullPath(folder), "dev-" + Guid.NewGuid());

    // ---- Runtime RPC (the Host inside the game) ----------------------------------------------

    private static HttpRequestMessage Request(JsonObject index, string operation, JsonObject? body)
    {
        var url = new Uri($"http://127.0.0.1:{index["port"]!.GetValue<int>()}/{operation}");
        var request = new HttpRequestMessage(body is null ? HttpMethod.Get : HttpMethod.Post, url);
        request.Headers.Authorization = new AuthenticationHeaderValue("Bearer", index["token"]!.GetValue<string>());
        if (body is not null) request.Content = new StringContent(body.ToJsonString(), Encoding.UTF8, "application/json");
        return request;
    }

    private static async Task<JsonNode> SendAsync(HttpRequestMessage request)
    {
        using var response = await Http.SendAsync(request, HttpCompletionOption.ResponseHeadersRead);
        string text = await response.Content.ReadAsStringAsync();
        if (text.Length > 4 * 1024 * 1024) throw new InvalidDataException("宿主响应过大");
        var result = JsonNode.Parse(text)!;
        if (!response.IsSuccessStatusCode) throw new InvalidOperationException(result["error"]?.GetValue<string>() ?? response.ReasonPhrase);
        return result;
    }

    // Full host status, or null when the game is not running with EFML.
    public static async Task<JsonObject?> HostStatusAsync()
    {
        try
        {
            using var request = Request(LoadIndex(), "status", null);
            return (await SendAsync(request)).AsObject();
        }
        catch (Exception exception) when (exception is HttpRequestException or TaskCanceledException or
                                              InvalidDataException or IOException or InvalidOperationException or
                                              System.Text.Json.JsonException)
        {
            return null;
        }
    }

    public static async Task<JsonNode> RuntimeAsync(string operation, string id, JsonNode? payload = null, string? requestId = null)
    {
        var index = LoadIndex();
        _ = Record(id);
        if (operation == "status")
        {
            using var statusRequest = Request(index, "status", null);
            var result = await SendAsync(statusRequest);
            var module = result["modules"]?.AsArray().FirstOrDefault(m => m?["id"]?.GetValue<string>() == id)?.DeepClone();
            return new JsonObject { ["connected"] = result["connected"]?.DeepClone() ?? true, ["module"] = module };
        }
        var body = new JsonObject { ["module_id"] = id };
        if (operation == "send")
        {
            body["request_id"] = requestId ?? Guid.NewGuid().ToString();
            body["body"] = payload?.DeepClone();
        }
        if (operation == "configure") body["configuration"] = payload?.DeepClone();
        using var request = Request(index, operation, body);
        return await SendAsync(request);
    }
}
