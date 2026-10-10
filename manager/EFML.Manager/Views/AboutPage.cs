using EFML.Manager.Services;
using Microsoft.UI.Xaml.Controls;

namespace EFML.Manager.Views;

internal sealed class AboutPage : UserControl
{
    public AboutPage()
    {
        Content = Ui.Page(Ui.Column(16,
            Ui.Title("关于 EFML"),
            Ui.Card(Ui.Column(8,
                Ui.Text($"EFML {AppPaths.Version} · Endfield Mod Loader", 16),
                Ui.Text("《明日方舟：终末地》模组加载器与管理器。只负责加载模组：游戏内的加载器、IL2CPP 解析、共享 Hook 链和模块宿主，加上这个管理器。"),
                Ui.Row(
                    Ui.Button("GitHub", () => Shell.OpenUrl("https://github.com/guchang233/EFML")),
                    Ui.Button("模组开发文档", () => Shell.OpenUrl("https://github.com/guchang233/EFML/blob/main/docs/MODULE-API.md"))))),
            Ui.Card(Ui.Column(8,
                Ui.Text("基于 Better-Endfield", 16),
                Ui.Text("EFML 的加载器、宿主与模块接口提取自 Better-Endfield（Better Endfield Next 4.0.0），与它的第三方模块二进制兼容：为 Better-Endfield 3.x 或 Next 4.x 编译的模块可以直接安装。"),
                Ui.Button("Better-Endfield", () => Shell.OpenUrl("https://github.com/Dr-hydra/Better-Endfield")))),
            Ui.Card(Ui.Column(8,
                Ui.Text("许可与免责声明", 16),
                Ui.Text("EFML 以 AGPL-3.0 许可发布。第三方组件：MinHook（BSD-2-Clause）、nlohmann/json（MIT）。"),
                Ui.Text("模组会向游戏进程注入代码，可能违反游戏服务条款并导致账号受限，请自行评估风险。EFML 不会关闭或绕过反作弊。本项目与鹰角网络及 Better-Endfield 作者无关。")))));
    }
}
