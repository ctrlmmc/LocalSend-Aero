#include "i18n.h"

#include <QCoreApplication>
#include <QHash>
#include <QLocale>
#include <QTranslator>

namespace i18n {
namespace {

struct Pair
{
    const char *zh;
    const char *en;
};

// One entry per distinct tr() source string. Keep the Chinese side identical
// to the .cpp literal, otherwise the lookup silently misses it and that row
// stays Chinese in the English UI.
const Pair kStrings[] = {
    { " (%1/s，剩余 %2)", " (%1/s, %2 left)" },
    { "%1 — %2 (%3, 端口 %4)", "%1 — %2 (%3, port %4)" },
    { "%1 (%2 KB)", "%1 (%2 KB)" },
    { "%1 B", "%1 B" },
    { "%1 GB", "%1 GB" },
    { "%1 KB", "%1 KB" },
    { "%1 MB", "%1 MB" },
    { "%1 发来一条文本消息", "Text message from %1" },
    { "%1 分 %2 秒", "%1 min %2 s" },
    { "%1 秒", "%1 s" },
    { "%1 想向本机发送 %2 个文件", "%1 wants to send %2 file(s) to this device" },
    { "%1 小时 %2 分", "%1 h %2 min" },
    { "%1 要求输入 PIN 码：", "%1 requires a PIN:" },
    { "LocalSend Aero 仍在后台运行，单击托盘图标可以重新打开窗口。",
      "LocalSend Aero is still running in the background. Click the tray icon to reopen the window." },
    { "PIN 码不正确", "Incorrect PIN" },
    { "PIN 码尝试次数过多，请稍后再试", "Too many PIN attempts, please try again later" },
    { "保存到：", "Save to:" },
    { "保存位置：", "Save location:" },
    { "成功", "Done" },
    { "传输来源与会话不符", "Transfer origin does not match the session" },
    { "传输历史", "Transfer history" },
    { "传输凭据无效", "Invalid transfer token" },
    { "窗口最大化时将标题文本颜色改为白色 (兼容 Windows Vista 熄灯效果)",
      "Use white title text when the window is maximized (for the Windows Vista lights-out frame)" },
    { "大小", "Size" },
    { "等待 %1 确认接收…", "Waiting for %1 to accept…" },
    { "等待 %1 确认接收… (点取消可中止)", "Waiting for %1 to accept… (Cancel to abort)" },
    { "对方拒绝接收", "The receiver declined" },
    { "对方没有在 30 秒内确认接收 (可能正在弹窗询问)",
      "The receiver did not answer within 30 seconds (it may be showing a prompt)" },
    { "对方取消了传输，未完成的文件已丢弃。", "The sender cancelled; unfinished files were deleted." },
    { "对方取消了发送。", "The sender cancelled." },
    { "对方中断了接收 (对端可能无法写入保存位置)",
      "The receiver aborted (it may not be able to write to the save folder)" },
    { "发送", "Send" },
    { "发送方中断了传输", "The sender aborted the transfer" },
    { "发送失败", "Send failed" },
    { "发送文本", "Send text" },
    { "发送文件", "Send files" },
    { "发现服务启动失败：", "Failed to start discovery: " },
    { "方向", "Direction" },
    { "否", "No" },
    { "复制", "Copy" },
    { "该文件已接收过", "This file was already received" },
    { "跟随系统", "Follow system" },
    { "更改", "Change" },
    { "关闭", "Close" },
    { "会话已失效", "The session has expired" },
    { "加密传输、字体与语言设置将在重新启动程序后生效。",
      "Encryption, font and language changes take effect after a restart." },
    { "监听端口 %1 (%2)", "Listening on port %1 (%2)" },
    { "简体中文", "简体中文" },
    { "将保存到 %1", "Will be saved to %1" },
    { "接收", "Receive" },
    { "接收超时：发送方没有继续传输数据。", "Receive timed out: the sender stopped sending data." },
    { "接收方报错：%1", "Receiver error: %1" },
    { "接收方返回状态 %1", "Receiver returned status %1" },
    { "接收方拒绝了本次发送：%1", "The receiver rejected this transfer: %1" },
    { "接收方没有接受任何文件", "The receiver accepted no files" },
    { "接收方无法保存文件 (对端磁盘空间不足或没有写入权限)",
      "The receiver cannot save the files (no space left or no write permission)" },
    { "接收方响应无法解析", "Unparsable response from the receiver" },
    { "接收时要求 PIN 码", "Require a PIN for receiving" },
    { "接收文本", "Receive text" },
    { "接收文件", "Receive files" },
    { "接收者已取消", "The receiver cancelled" },
    { "接收中断：%1", "Receive interrupted: %1" },
    { "接受", "Accept" },
    { "结果", "Result" },
    { "界面语言：", "Language:" },
    { "拒绝", "Decline" },
    { "登录时自动启动 (最小化到托盘)", "Start at login (minimized to tray)" },
    { "历史", "History" },
    { "没有可发送的文件", "Nothing to send" },
    { "没有可用的网络接口", "No usable network interface" },
    { "每次接收前询问保存位置", "Ask for the save location before each transfer" },
    { "清空历史", "Clear history" },
    { "取消", "Cancel" },
    { "取消发送", "Cancel sending" },
    { "取消接收", "Cancel receiving" },
    { "上一次发送还在收尾，请稍后再试", "The previous transfer is still finishing, try again in a moment" },
    { "设备", "Device" },
    { "设备已离线", "The device is offline" },
    { "设备已离线，无法连接 %1", "The device is offline, cannot reach %1" },
    { "设备证书生成失败", "Could not create the device certificate" },
    { "设置", "Settings" },
    { "失败", "Failed" },
    { "时间", "Time" },
    { "使用加密传输 (HTTPS)", "Use encrypted transfer (HTTPS)" },
    { "是", "Yes" },
    { "刷新", "Refresh" },
    { "刷新列表", "Refresh list" },
    { "添加设备", "Add device" },
    { "输入对方的 IP 地址（可带端口，如 192.168.1.5:53317）",
      "Enter the address of the other device (port optional, e.g. 192.168.1.5:53317)" },
    { "无法连接到 %1:%2。请确认对方已经运行、地址正确，并且两台电脑在同一网络。",
      "Could not reach %1:%2. Check that the other device is running, the address is right and both are on the same network." },
    { "已添加设备 %1。", "Added %1." },
    { "退出", "Quit" },
    { "完成", "Done" },
    { "网络未启动", "Network not started" },
    { "文本将作为一条消息发送给选中的设备。", "The text is sent as a single message to the selected device." },
    { "文本内容已送达 %1 (对方可能仍在弹窗确认)",
      "Text delivered to %1 (the receiver may still be showing a prompt)" },
    { "文本消息：%1", "Text message: %1" },
    { "文本已复制到剪贴板。", "Text copied to the clipboard." },
    { "文本已送达 %1 (对方关闭了确认窗口)", "Text delivered to %1 (the prompt was closed)" },
    { "文本已送达 %1 (对方已关闭确认窗口)", "Text delivered to %1 (the prompt was closed)" },
    { "文件", "Files" },
    { "无法监听端口：", "Cannot listen on port: " },
    { "无法写入保存位置：%1", "Cannot write to the save location: %1" },
    { "系统默认", "System default" },
    { "显示主窗口", "Show main window" },
    { "需要 PIN 码", "PIN required" },
    { "选择保存位置", "Choose a save location" },
    { "选择文件", "Choose files" },
    { "选择文件或把文件拖进窗口，也可以发一段文字；附近的 LocalSend 设备会出现在下面的列表里。",
      "Pick files or drop them onto the window, or send a piece of text. Nearby LocalSend devices show up in the list below." },
    { "选择要发送的文件", "Choose the files to send" },
    { "已从 %1 接收 %2 个文件 (共 %3)", "Received %2 file(s) from %1 (%3)" },
    { "已发送 %1 / %2", "Sent %1 / %2" },
    { "已发现 %1 台设备", "%1 device(s) found" },
    { "已取消", "Cancelled" },
    { "已取消发送", "Sending cancelled" },
    { "所选设备已离线，请刷新列表后重试。",
      "The selected device is offline; refresh the list and try again." },
    { "已取消接收，未完成的文件已丢弃。", "Receiving cancelled; unfinished files were deleted." },
    { "已向 %1 发送 %2 个文件", "Sent %2 file(s) to %1" },
    { "已有发送任务在进行", "Another transfer is already running" },
    { "应用字体：", "Font:" },
    { "在局域网内与 LocalSend 设备互传文件", "Share files with LocalSend devices on your local network" },
    { "正在发送 %1", "Sending %1" },
    { "正在接收 %1 / %2", "Receiving %1 / %2" },
    { "正在接收 %1 个文件到 %2", "Receiving %1 file(s) into %2" },
    { "正在接收文件，请等接收完成后再发送", "Still receiving, wait until it finishes before sending" },
    { "正在取消发送…", "Cancelling…" },
    { "正在使用 PIN 码重试…", "Retrying with the PIN…" },
    { "正在搜索附近的设备…", "Searching for nearby devices…" },
    { "正在与 %1 建立会话…", "Setting up a session with %1…" },
    { "指纹：%1\n版本：%2\n设备类型：%3", "Fingerprint: %1\nVersion: %2\nDevice type: %3" },
    { "中断", "Interrupted" },
    { "最近 200 条收发记录。", "The 200 most recent transfers." },
    // Added together with the folder / history work.
    { "选择文件夹", "Choose folder" },
    { "选择要发送的文件夹", "Choose the folder to send" },
    { "打开所在文件夹", "Open containing folder" },
    { "重新发送", "Send again" },
    { "打开", "Open" },
    { "文件已不存在", "The file no longer exists" },
    { "文件已不存在，可能已被移动或删除。", "The file no longer exists; it may have been moved or deleted." },
    { "空文件夹", "Empty folder" },
    { "这个文件夹里没有可以发送的文件。", "There is nothing to send inside this folder." },
    { "正在传输", "Transfer in progress" },
    { "请等这次传输结束后再添加。", "Please add items once the current transfer has finished." },
    { "有 %1 个文件已不存在，只加入仍能找到的 %2 个。", "%1 file(s) no longer exist, only the %2 still available were added." },
    { "确定要删除全部传输记录吗？", "Delete the whole transfer history?" },
    { "清空", "Clear" },
    { "总计 %1 / %2", "Total %1 / %2" },
    { "此选项在 Aero 禁用时无效。", "This option has no effect while Aero is disabled." },
    // Device name (settings) and the transfer icon round.
    { "本机名称：", "Device name:" },
    { "本机名称改完立即生效；加密传输、字体与语言设置将在重新启动程序后生效。",
      "The device name applies immediately; HTTPS, font and language changes need a restart." },
    { "无法连接到 %1:%2", "Cannot reach %1:%2" },
    { "请确认对方已经运行、地址正确，并且两台电脑在同一网络。",
      "Make sure the other device is running, the address is right, and both are on the same network." },
    // Send-side failures that used to fall back to Chinese only.
    { "确定", "OK" },
    { "无法与 %1 建立连接：%2", "Cannot connect to %1: %2" },
    { "无法与 %1 建立安全连接，对方可能正在处理上一次传输，请稍后重试",
      "Cannot establish a secure connection to %1; it may still be finishing the previous transfer, please retry in a moment" },
    { "对方与本案使用同一套证书（很可能是同一个安装副本），请删除其中一个副本的 certs 目录后重试",
      "The peer uses the same certificate as this copy (probably a duplicated install); delete the certs folder of one copy and retry" },
};

const QHash<QString, QString> &table()
{
    static QHash<QString, QString> hash;
    if (hash.isEmpty()) {
        const int count = static_cast<int>(sizeof(kStrings) / sizeof(kStrings[0]));
        for (int i = 0; i < count; ++i)
            hash.insert(QString::fromUtf8(kStrings[i].zh), QString::fromUtf8(kStrings[i].en));
    }
    return hash;
}

class Translator : public QTranslator
{
public:
    QString translate(const char *context, const char *sourceText, const char *disambiguation,
                      int n) const override
    {
        // The app has a single namespace, so the context is not part of the key.
        Q_UNUSED(context);
        Q_UNUSED(disambiguation);
        Q_UNUSED(n);
        if (!sourceText || !*sourceText)
            return QString();
        return table().value(QString::fromUtf8(sourceText));
    }

    bool isEmpty() const override { return false; }
};

Translator *g_translator = 0;
QString g_active = QStringLiteral("zh");

bool systemPrefersEnglish()
{
    const QStringList ui = QLocale::system().uiLanguages();
    for (int i = 0; i < ui.size(); ++i) {
        if (ui.at(i).startsWith(QLatin1String("en"), Qt::CaseInsensitive))
            return true;
        if (ui.at(i).startsWith(QLatin1String("zh"), Qt::CaseInsensitive))
            return false;
    }
    return QLocale::system().language() == QLocale::English;
}

} // namespace

void install(const QString &language)
{
    QString wanted = language.trimmed().toLower();
    if (wanted.isEmpty() || wanted == QLatin1String("system"))
        wanted = systemPrefersEnglish() ? QStringLiteral("en") : QStringLiteral("zh");

    QCoreApplication *app = QCoreApplication::instance();
    if (!app)
        return;

    if (wanted == QLatin1String("en")) {
        if (!g_translator)
            g_translator = new Translator;
        app->installTranslator(g_translator);
        g_active = QStringLiteral("en");
    } else if (g_translator) {
        app->removeTranslator(g_translator);
        g_active = QStringLiteral("zh");
    } else {
        g_active = QStringLiteral("zh");
    }
}

QString activeLanguage()
{
    return g_active;
}

} // namespace i18n
