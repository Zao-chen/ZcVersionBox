#include "macos_services.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QString>
#include <QTextStream>

#import <AppKit/AppKit.h>

namespace
{

QString xmlEscaped(QString text)
{
    text.replace("&", "&amp;");
    text.replace("<", "&lt;");
    text.replace(">", "&gt;");
    text.replace("\"", "&quot;");
    text.replace("'", "&apos;");
    return text;
}

QString workflowPath()
{
    const QString home = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    if (home.isEmpty()) {
        return {};
    }
    return home + "/Library/Services/添加到 ZcVersionBox.workflow";
}

bool writeTextFile(const QString &path, const QString &content)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        return false;
    }

    QTextStream out(&file);
    out << content;
    return true;
}

bool installFinderQuickAction()
{
    const QString path = workflowPath();
    if (path.isEmpty()) {
        return false;
    }

    QDir dir;
    if (!dir.mkpath(path + "/Contents/Resources")) {
        return false;
    }

    const QString executablePath = QCoreApplication::applicationFilePath();
    const QString script = QString(
        "for f in \"$@\"\n"
        "do\n"
        "    \"%1\" \"$f\" >/dev/null 2>&1 &\n"
        "done\n")
        .arg(executablePath);

    const QString infoPlist = QStringLiteral(R"(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key>
    <string>zh_CN</string>
    <key>CFBundleIdentifier</key>
    <string>com.zc.versionbox.quickaction</string>
    <key>CFBundleName</key>
    <string>添加到 ZcVersionBox</string>
    <key>CFBundleShortVersionString</key>
    <string>1.0</string>
    <key>NSServices</key>
    <array>
        <dict>
            <key>NSMenuItem</key>
            <dict>
                <key>default</key>
                <string>添加到 ZcVersionBox</string>
            </dict>
            <key>NSMessage</key>
            <string>runWorkflowAsService</string>
            <key>NSRequiredContext</key>
            <dict>
                <key>NSApplicationIdentifier</key>
                <string>com.apple.finder</string>
            </dict>
            <key>NSSendFileTypes</key>
            <array>
                <string>public.item</string>
            </array>
        </dict>
    </array>
</dict>
</plist>
)");

    const QString document = QString(R"(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>AMApplicationBuild</key>
    <string>521</string>
    <key>AMApplicationVersion</key>
    <string>2.10</string>
    <key>AMDocumentVersion</key>
    <string>2</string>
    <key>actions</key>
    <array>
        <dict>
            <key>action</key>
            <dict>
                <key>ActionBundlePath</key>
                <string>/System/Library/Automator/Run Shell Script.action</string>
                <key>ActionName</key>
                <string>Run Shell Script</string>
                <key>ActionParameters</key>
                <dict>
                    <key>COMMAND_STRING</key>
                    <string>%1</string>
                    <key>CheckedForUserDefaultShell</key>
                    <true/>
                    <key>inputMethod</key>
                    <integer>1</integer>
                    <key>shell</key>
                    <string>/bin/sh</string>
                    <key>source</key>
                    <string></string>
                </dict>
                <key>AMAccepts</key>
                <dict>
                    <key>Container</key>
                    <string>List</string>
                    <key>Optional</key>
                    <true/>
                    <key>Types</key>
                    <array>
                        <string>com.apple.cocoa.path</string>
                    </array>
                </dict>
                <key>AMActionVersion</key>
                <string>2.0.3</string>
                <key>AMProvides</key>
                <dict>
                    <key>Container</key>
                    <string>List</string>
                    <key>Types</key>
                    <array>
                        <string>com.apple.cocoa.path</string>
                    </array>
                </dict>
                <key>BundleIdentifier</key>
                <string>com.apple.RunShellScript</string>
                <key>CanShowSelectedItemsWhenRun</key>
                <false/>
                <key>CanShowWhenRun</key>
                <true/>
                <key>Class Name</key>
                <string>RunShellScriptAction</string>
                <key>UUID</key>
                <string>5D6016EC-39AB-49D5-8E35-2DC03A0A14C2</string>
                <key>isViewVisible</key>
                <true/>
            </dict>
            <key>isViewVisible</key>
            <true/>
        </dict>
    </array>
    <key>connectors</key>
    <dict/>
    <key>workflowMetaData</key>
    <dict>
        <key>serviceApplicationBundleID</key>
        <string>com.apple.finder</string>
        <key>serviceInputTypeIdentifier</key>
        <string>com.apple.Automator.fileSystemObject</string>
        <key>serviceOutputTypeIdentifier</key>
        <string>com.apple.Automator.nothing</string>
        <key>serviceProcessesInput</key>
        <integer>0</integer>
        <key>workflowTypeIdentifier</key>
        <string>com.apple.Automator.servicesMenu</string>
    </dict>
</dict>
</plist>
)")
                                 .arg(xmlEscaped(script));

    return writeTextFile(path + "/Contents/Info.plist", infoPlist)
           && writeTextFile(path + "/Contents/Resources/document.wflow", document);
}

void removeFinderQuickAction()
{
    const QString path = workflowPath();
    if (!path.isEmpty()) {
        QDir(path).removeRecursively();
    }
}

} // namespace

void setMacServicesProviderEnabled(bool enabled)
{
    if (enabled) {
        installFinderQuickAction();
    } else {
        removeFinderQuickAction();
    }

    NSUpdateDynamicServices();
}
