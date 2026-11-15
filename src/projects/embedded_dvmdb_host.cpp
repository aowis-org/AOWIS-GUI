#include "projects/embedded_dvmdb_host.h"

#ifdef AOWIS_STANDALONE_DVMDB_ENABLED
#include <aowis/dvmdb/dvmdb_http_routes.h>
#include <aowis/dvmdb/project_service.h>
#include <httplib.h>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace
{
class LocalService final
{
public:
    explicit LocalService(const QString &path)
        // UTF-16 -> std::filesystem::path is lossless on every platform. A
        // narrow toStdString() would be reinterpreted in the ANSI code page
        // by some Windows standard libraries (non-ASCII user profile paths).
        : service_(std::filesystem::path(path.toStdU16String()))
    {
        aowis::dvmdb::http::registerRoutes(this->server_, this->service_);
        const int port = this->server_.bind_to_any_port("127.0.0.1");
        if (port <= 0)
            throw std::runtime_error(QCoreApplication::translate(
                "EmbeddedDvmdbHost", "Cannot bind the internal DVMDB loopback listener.").toStdString());
        this->endpoint_ = QStringLiteral("http://127.0.0.1:%1").arg(port);
        this->thread_ = std::thread([this] { this->server_.listen_after_bind(); });
        this->server_.wait_until_ready();
        if (!this->server_.is_running())
        {
            this->server_.stop();
            if (this->thread_.joinable()) this->thread_.join();
            throw std::runtime_error(QCoreApplication::translate(
                "EmbeddedDvmdbHost", "The internal DVMDB listener could not start.").toStdString());
        }
    }

    ~LocalService()
    {
        this->server_.stop();
        if (this->thread_.joinable()) this->thread_.join();
    }

    QString endpoint() const { return this->endpoint_; }
    QString repositoryId() const { return QString::fromStdString(this->service_.repositoryId()); }

    void requirePrivate()
    {
        const aowis::dvmdb::RepositoryRole role = this->service_.repositoryRole();
        if (role == aowis::dvmdb::RepositoryRole::Shared)
            throw std::runtime_error(QCoreApplication::translate(
                "EmbeddedDvmdbHost", "This local DVMDB is configured as shared, not private.").toStdString());
        if (role == aowis::dvmdb::RepositoryRole::Unconfigured)
            this->service_.configureRepositoryRole(aowis::dvmdb::RepositoryRole::Private);
    }

private:
    // The service must outlive every registered route and its request threads.
    aowis::dvmdb::ProjectService service_;
    httplib::Server server_;
    std::thread thread_;
    QString endpoint_;
};

class LocalServices final
{
public:
    QString endpoint(const QString &path, bool personal, QString *repository_id)
    {
        std::lock_guard<std::mutex> guard(this->mutex_);
        std::map<QString, std::unique_ptr<LocalService>>::iterator entry = this->services_.find(path);
        if (entry == this->services_.end())
        {
            std::unique_ptr<LocalService> instance = std::make_unique<LocalService>(path);
            entry = this->services_.emplace(path, std::move(instance)).first;
        }
        if (personal) entry->second->requirePrivate();
        if (repository_id) *repository_id = entry->second->repositoryId();
        return entry->second->endpoint();
    }

private:
    std::mutex mutex_;
    std::map<QString, std::unique_ptr<LocalService>> services_;
};
}

QString embeddedDvmdbEndpoint(const QString &storage_path, bool personal,
                              QString *repository_id, QString *error)
{
    if (repository_id) repository_id->clear();
    if (error) error->clear();
    const QString path = QDir::cleanPath(storage_path);
    // Refuse a filesystem root ("/" or a Windows drive root such as "C:/").
    if (!QDir::isAbsolutePath(path) || path.isEmpty() || QDir(path).isRoot())
    {
        if (error) *error = QCoreApplication::translate(
            "EmbeddedDvmdbHost", "Choose a dedicated absolute local DVMDB data directory.");
        return {};
    }
    try
    {
        static LocalServices services;
        return services.endpoint(path, personal, repository_id);
    }
    catch (const std::exception &exception)
    {
        if (error) *error = QString::fromUtf8(exception.what());
        return {};
    }
}
#endif
