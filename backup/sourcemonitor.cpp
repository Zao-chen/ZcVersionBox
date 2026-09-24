#include "sourcemonitor.h"
#include "types.h"
#include <QDir>
#include <QFileInfo>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#ifdef Q_OS_MACOS
#include <CoreServices/CoreServices.h>
#elif defined(Q_OS_WIN)
#include <windows.h>
#endif

namespace Backup
{
class NativeSourceMonitor final : public SourceMonitor
{
  public:
    NativeSourceMonitor(QString source, QObject *parent) : SourceMonitor(parent), m_source(std::move(source)) {}
    ~NativeSourceMonitor() override { stop(); }
    void start() override
    {
        if (m_thread.joinable())
            return;
        m_stop = false;
        m_thread = std::thread([this]
                               {
                                   run();
                               });
    }
    void stop() override
    {
        m_stop = true;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
#ifdef Q_OS_WIN
            if (m_directory != INVALID_HANDLE_VALUE)
                CancelIoEx(m_directory, nullptr);
#endif
        }
        m_wakeup.notify_all();
        if (m_thread.joinable())
            m_thread.join();
    }

  private:
    QString m_source;
    std::thread m_thread;
    std::atomic_bool m_stop{false};
    std::mutex m_mutex;
    std::condition_variable m_wakeup;
#ifdef Q_OS_MACOS
    static void callback(ConstFSEventStreamRef, void *context, size_t count, void *eventPaths, const FSEventStreamEventFlags flags[], const FSEventStreamEventId[])
    {
        auto self = static_cast<NativeSourceMonitor *>(context);
        QStringList paths;
        bool lost = false;
        auto native = static_cast<char **>(eventPaths);
        for (size_t i = 0; i < count; ++i)
        {
            if (flags[i] & (kFSEventStreamEventFlagMustScanSubDirs | kFSEventStreamEventFlagUserDropped | kFSEventStreamEventFlagKernelDropped | kFSEventStreamEventFlagEventIdsWrapped | kFSEventStreamEventFlagRootChanged | kFSEventStreamEventFlagUnmount))
                lost = true;
            const auto path = normalizedPath(QString::fromUtf8(native[i]));
            if (containsPath(self->m_source, path))
                paths.append(path);
            else if (containsPath(path, self->m_source))
                paths.append(self->m_source);
        }
        if (lost || !paths.isEmpty())
            emit self->changed(paths, lost);
    }
#elif defined(Q_OS_WIN)
    HANDLE m_directory = INVALID_HANDLE_VALUE;
#endif
    void run()
    {
        QString parent = QFileInfo(m_source).absolutePath();
        while (!QFileInfo(parent).isDir())
        {
            const auto next = QFileInfo(parent).absolutePath();
            if (next == parent)
            {
                emit failed("无法找到可监听的父目录");
                return;
            }
            parent = next;
        }
#ifdef Q_OS_MACOS
        auto path = CFStringCreateWithCharacters(nullptr, reinterpret_cast<const UniChar *>(parent.utf16()), parent.size());
        const void *values[] = {path};
        auto paths = CFArrayCreate(nullptr, values, 1, &kCFTypeArrayCallBacks);
        FSEventStreamContext context{0, this, nullptr, nullptr, nullptr};
        auto stream = FSEventStreamCreate(nullptr, &callback, &context, paths, kFSEventStreamEventIdSinceNow, 0.15, kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagWatchRoot | kFSEventStreamCreateFlagNoDefer);
        CFRelease(paths);
        CFRelease(path);
        if (!stream)
        {
            emit failed("无法创建 FSEvents 监听");
            return;
        }
        auto queue = dispatch_queue_create("com.zcversionbox.source-monitor", DISPATCH_QUEUE_SERIAL);
        FSEventStreamSetDispatchQueue(stream, queue);
        if (!FSEventStreamStart(stream))
        {
            FSEventStreamInvalidate(stream);
            FSEventStreamRelease(stream);
            dispatch_release(queue);
            emit failed("无法启动 FSEvents，请检查目录访问权限");
            return;
        }
        emit ready();
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_wakeup.wait(lock, [this]
                          {
                              return m_stop.load();
                          });
        }
        FSEventStreamStop(stream);
        FSEventStreamInvalidate(stream);
        FSEventStreamRelease(stream);
        dispatch_sync_f(queue, nullptr, [](void *){
                                        });
        dispatch_release(queue);
#elif defined(Q_OS_WIN)
        HANDLE directory = CreateFileW(reinterpret_cast<LPCWSTR>(parent.utf16()), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        if (directory == INVALID_HANDLE_VALUE)
        {
            emit failed("无法启动目录监听，错误码：" + QString::number(GetLastError()));
            return;
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_directory = directory;
        }
        HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!event)
        {
            CloseHandle(directory);
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_directory = INVALID_HANDLE_VALUE;
            }
            emit failed("无法创建目录监听事件");
            return;
        }
        alignas(DWORD) unsigned char buffer[65536];
        bool announced = false;
        while (!m_stop)
        {
            OVERLAPPED overlap{};
            overlap.hEvent = event;
            ResetEvent(event);
            if (!ReadDirectoryChangesW(directory, buffer, sizeof(buffer), TRUE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_ATTRIBUTES, nullptr, &overlap, nullptr))
            {
                emit failed("目录监听失败，错误码：" + QString::number(GetLastError()));
                break;
            }
            if (!announced)
            {
                announced = true;
                emit ready();
            }
            while (!m_stop && WaitForSingleObject(event, 100) == WAIT_TIMEOUT)
            {
            }
            if (m_stop)
            {
                CancelIoEx(directory, &overlap);
                WaitForSingleObject(event, INFINITE);
                break;
            }
            DWORD count = 0;
            if (!GetOverlappedResult(directory, &overlap, &count, FALSE))
            {
                if (GetLastError() == ERROR_NOTIFY_ENUM_DIR)
                {
                    emit changed({m_source}, true);
                    continue;
                }
                emit failed("目录监听中断，错误码：" + QString::number(GetLastError()));
                break;
            }
            if (!count)
            {
                emit changed({m_source}, true);
                continue;
            }
            QStringList paths;
            DWORD offset = 0;
            while (offset < count)
            {
                const auto entry = reinterpret_cast<FILE_NOTIFY_INFORMATION *>(buffer + offset);
                const auto relative = QString::fromWCharArray(entry->FileName, entry->FileNameLength / sizeof(WCHAR));
                const auto path = normalizedPath(QDir(parent).filePath(relative));
                if (containsPath(m_source, path))
                    paths.append(path);
                else if (containsPath(path, m_source))
                    paths.append(m_source);
                if (!entry->NextEntryOffset)
                    break;
                offset += entry->NextEntryOffset;
            }
            if (!paths.isEmpty())
                emit changed(paths, false);
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_directory = INVALID_HANDLE_VALUE;
            CloseHandle(directory);
        }
        CloseHandle(event);
#else
        emit failed("当前平台没有原生文件监控实现");
#endif
    }
};
SourceMonitor *SourceMonitor::create(const QString &source, QObject *parent) { return new NativeSourceMonitor(source, parent); }
} // namespace Backup
