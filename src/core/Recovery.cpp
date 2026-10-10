#include "core/Recovery.h"

#include <algorithm>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSettings>
#include <QStandardPaths>

#include "core/Io.h"

namespace tessel::recovery {

namespace {

// Empty unless a caller pointed the module somewhere else.
QString& rootOverride()
{
    static QString directory;
    return directory;
}

const char* const kLockName = "session.lock";
const char* const kSessionPrefix = "session-";

QString documentName(int key)
{
    return QStringLiteral("doc-%1.%2").arg(key).arg(QLatin1String(io::kNativeExtension));
}

QString metaName(int key)
{
    return QStringLiteral("doc-%1.json").arg(key);
}

// A claim on a session directory, taken only when the process that owned it is
// gone. Qt decides that by the pid in the lock file, so the stale timeout is
// switched off: a long autosave interval must never make a running session
// look dead.
std::shared_ptr<QLockFile> claimDirectory(const QString& directory)
{
    auto lock = std::make_shared<QLockFile>(QDir(directory).filePath(QLatin1String(kLockName)));
    lock->setStaleLockTime(0);
    if (!lock->tryLock(0))
        return nullptr;
    return lock;
}

Entry readEntry(const QString& directory, const QString& fileName)
{
    const QDir dir(directory);
    Entry entry;
    entry.path = dir.filePath(fileName);
    const QFileInfo info(entry.path);
    entry.size = info.size();
    entry.savedAt = info.lastModified();

    const QString meta = dir.filePath(QFileInfo(fileName).completeBaseName()
                                      + QStringLiteral(".json"));
    QFile file(meta);
    if (file.open(QIODevice::ReadOnly)) {
        const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
        entry.originalPath = object.value(QStringLiteral("originalPath")).toString();
        entry.title = object.value(QStringLiteral("title")).toString();
        const QDateTime savedAt = QDateTime::fromString(
            object.value(QStringLiteral("savedAt")).toString(), Qt::ISODate);
        if (savedAt.isValid())
            entry.savedAt = savedAt;
    }
    return entry;
}

} // namespace

QString root()
{
    if (!rootOverride().isEmpty())
        return rootOverride();
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/recovery");
}

void setRoot(const QString& directory)
{
    rootOverride() = directory;
}

QString Entry::displayName() const
{
    if (!title.isEmpty())
        return title;
    if (!originalPath.isEmpty())
        return QFileInfo(originalPath).fileName();
    return QFileInfo(path).fileName();
}

// ---------------------------------------------------------------- options

Options loadOptions(const QSettings& settings)
{
    Options options;
    options.autosave = settings.value(QStringLiteral("recovery/autosave"), true).toBool();
    options.autosaveMinutes = std::clamp(
        settings.value(QStringLiteral("recovery/autosaveMinutes"), 2).toInt(),
        kMinAutosaveMinutes, kMaxAutosaveMinutes);
    options.restoreSession =
        settings.value(QStringLiteral("recovery/restoreSession"), true).toBool();
    options.keepDays = std::clamp(
        settings.value(QStringLiteral("recovery/keepDays"), 14).toInt(),
        kMinKeepDays, kMaxKeepDays);
    return options;
}

void storeOptions(QSettings& settings, const Options& options)
{
    settings.setValue(QStringLiteral("recovery/autosave"), options.autosave);
    settings.setValue(QStringLiteral("recovery/autosaveMinutes"),
                      std::clamp(options.autosaveMinutes, kMinAutosaveMinutes,
                                 kMaxAutosaveMinutes));
    settings.setValue(QStringLiteral("recovery/restoreSession"), options.restoreSession);
    settings.setValue(QStringLiteral("recovery/keepDays"),
                      std::clamp(options.keepDays, kMinKeepDays, kMaxKeepDays));
}

// ---------------------------------------------------------------- session list

OpenDocuments loadOpenDocuments(const QSettings& settings)
{
    OpenDocuments documents;
    documents.paths = settings.value(QStringLiteral("session/openDocuments")).toStringList();
    documents.current = settings.value(QStringLiteral("session/current"), -1).toInt();
    if (documents.current >= documents.paths.size()) {
        documents.current = documents.paths.isEmpty()
            ? -1 : static_cast<int>(documents.paths.size()) - 1;
    }
    return documents;
}

void storeOpenDocuments(QSettings& settings, const OpenDocuments& documents)
{
    settings.setValue(QStringLiteral("session/openDocuments"), documents.paths);
    settings.setValue(QStringLiteral("session/current"), documents.current);
}

OpenDocuments existingOnly(const OpenDocuments& documents)
{
    OpenDocuments kept;
    for (int i = 0; i < documents.paths.size(); ++i) {
        if (!QFileInfo::exists(documents.paths.at(i)))
            continue;
        if (i == documents.current)
            kept.current = static_cast<int>(kept.paths.size());
        kept.paths.append(documents.paths.at(i));
    }
    if (kept.current < 0 && !kept.paths.isEmpty())
        kept.current = 0;
    return kept;
}

// ---------------------------------------------------------------- entries

std::vector<Abandoned> findAbandoned()
{
    std::vector<Abandoned> sessions;
    QDir dir(root());
    if (!dir.exists())
        return sessions;
    const QStringList names = dir.entryList(
        QStringList{QLatin1String(kSessionPrefix) + QStringLiteral("*")},
        QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString& name : names) {
        const QString path = dir.filePath(name);
        std::shared_ptr<QLockFile> claim = claimDirectory(path);
        if (!claim)
            continue; // that session is still running
        Abandoned session;
        session.directory = path;
        session.claim = std::move(claim);
        const QStringList files = QDir(path).entryList(
            QStringList{QStringLiteral("*.") + QLatin1String(io::kNativeExtension)},
            QDir::Files, QDir::Name);
        for (const QString& file : files)
            session.entries.push_back(readEntry(path, file));
        std::sort(session.entries.begin(), session.entries.end(),
                  [](const Entry& a, const Entry& b) { return a.savedAt > b.savedAt; });
        sessions.push_back(std::move(session));
    }
    std::sort(sessions.begin(), sessions.end(), [](const Abandoned& a, const Abandoned& b) {
        const QDateTime left = a.entries.empty() ? QDateTime() : a.entries.front().savedAt;
        const QDateTime right = b.entries.empty() ? QDateTime() : b.entries.front().savedAt;
        return left > right;
    });
    return sessions;
}

std::vector<Entry> allEntries(const std::vector<Abandoned>& sessions)
{
    std::vector<Entry> entries;
    for (const Abandoned& session : sessions)
        entries.insert(entries.end(), session.entries.begin(), session.entries.end());
    return entries;
}

void discard(std::vector<Abandoned>& sessions)
{
    for (Abandoned& session : sessions) {
        if (session.claim)
            session.claim->unlock();
        session.claim.reset();
        QDir(session.directory).removeRecursively();
    }
    sessions.clear();
}

int pruneOlderThan(int keepDays, const QDateTime& now)
{
    keepDays = std::clamp(keepDays, kMinKeepDays, kMaxKeepDays);
    std::vector<Abandoned> sessions = findAbandoned();
    std::vector<Abandoned> stale;
    for (Abandoned& session : sessions) {
        QDateTime newest;
        for (const Entry& entry : session.entries) {
            if (!newest.isValid() || entry.savedAt > newest)
                newest = entry.savedAt;
        }
        // A directory with nothing left in it is swept up as well: it is what
        // a discarded or recovered session leaves if a delete half failed.
        if (!newest.isValid() || newest.daysTo(now) >= keepDays)
            stale.push_back(std::move(session));
    }
    const int count = static_cast<int>(stale.size());
    discard(stale);
    return count;
}

// ---------------------------------------------------------------- session

Session::Session()
{
    QDir base(root());
    if (!base.mkpath(QStringLiteral(".")))
        return;
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    for (int attempt = 0; attempt < 64; ++attempt) {
        QString name = QStringLiteral("%1%2-%3")
                           .arg(QLatin1String(kSessionPrefix))
                           .arg(QCoreApplication::applicationPid())
                           .arg(stamp);
        if (attempt > 0)
            name += QStringLiteral("-%1").arg(attempt);
        const QString path = base.filePath(name);
        // An existing directory belongs to another session, running or
        // crashed, and its files must not be mixed with these ones.
        if (QFileInfo::exists(path))
            continue;
        if (!QDir().mkpath(path))
            continue;
        // The lock is held for the lifetime of the session, which is what
        // tells another instance that this one is alive.
        auto lock = std::unique_ptr<QLockFile>(
            new QLockFile(QDir(path).filePath(QLatin1String(kLockName))));
        lock->setStaleLockTime(0);
        if (!lock->tryLock(0))
            continue;
        directory_ = path;
        lock_ = std::move(lock);
        return;
    }
}

Session::~Session()
{
    if (!lock_)
        return;
    lock_->unlock();
    lock_.reset();
    QDir dir(directory_);
    // Nothing left to recover means the run ended cleanly, so the directory
    // goes too rather than piling up one empty folder per run.
    if (dir.exists() && dir.isEmpty())
        dir.removeRecursively();
}

QString Session::pathFor(int key) const
{
    if (directory_.isEmpty())
        return QString();
    return QDir(directory_).filePath(documentName(key));
}

bool Session::write(int key, const DocumentState& state, const QString& originalPath,
                    const QString& title, QString* error)
{
    if (!isOpen()) {
        if (error)
            *error = QCoreApplication::translate(
                "Core", "No recovery directory is available.");
        return false;
    }
    if (state.layers.empty() || state.width <= 0 || state.height <= 0) {
        if (error)
            *error = QCoreApplication::translate(
                "Core", "%1 has nothing to save.").arg(title);
        return false;
    }
    // A throwaway document, so the existing writer can be used as it is. It is
    // built here rather than passed in because the caller is on another thread
    // and must not touch the document it snapshotted.
    Document doc(state.width, state.height);
    doc.restore(state);
    if (!io::saveRecoveryCopy(doc, pathFor(key), error))
        return false;

    QJsonObject object;
    object[QStringLiteral("originalPath")] = originalPath;
    object[QStringLiteral("title")] = title;
    object[QStringLiteral("savedAt")] =
        QDateTime::currentDateTime().toString(Qt::ISODate);
    QFile meta(QDir(directory_).filePath(metaName(key)));
    if (meta.open(QIODevice::WriteOnly | QIODevice::Truncate))
        meta.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
    return true;
}

void Session::drop(int key)
{
    if (directory_.isEmpty())
        return;
    QFile::remove(pathFor(key));
    QFile::remove(QDir(directory_).filePath(metaName(key)));
}

void Session::dropAll()
{
    if (directory_.isEmpty())
        return;
    QDir dir(directory_);
    const QStringList files = dir.entryList(QDir::Files);
    for (const QString& file : files) {
        if (file == QLatin1String(kLockName))
            continue;
        QFile::remove(dir.filePath(file));
    }
}

// ---------------------------------------------------------------- autosaver

Autosaver::Autosaver(QObject* parent) : QObject(parent)
{
    timer_.setSingleShot(false);
    connect(&timer_, &QTimer::timeout, this, &Autosaver::saveNow);
    restartTimer();
}

Autosaver::~Autosaver()
{
    if (worker_.joinable())
        worker_.join();
}

void Autosaver::setSource(Source source)
{
    source_ = std::move(source);
}

void Autosaver::setIntervalMinutes(int minutes)
{
    minutes_ = std::clamp(minutes, kMinAutosaveMinutes, kMaxAutosaveMinutes);
    restartTimer();
}

void Autosaver::setEnabled(bool enabled)
{
    enabled_ = enabled;
    restartTimer();
}

int Autosaver::millisecondsToNextWrite() const
{
    return timer_.isActive() ? timer_.remainingTime() : -1;
}

void Autosaver::restartTimer()
{
    timer_.stop();
    if (!enabled_)
        return;
    timer_.start(minutes_ * 60 * 1000);
}

void Autosaver::saveNow()
{
    if (pending_ || !source_)
        return;
    std::vector<Request> requests = source_();
    auto batch = std::make_shared<Batch>();
    for (Request& request : requests) {
        if (!request.snapshot)
            continue;
        const auto known = written_.find(request.key);
        if (known != written_.end() && known->second == request.revision)
            continue; // unchanged since the last write
        Job job;
        job.key = request.key;
        job.revision = request.revision;
        job.originalPath = request.originalPath;
        job.title = request.title;
        job.state = request.snapshot();
        batch->jobs.push_back(std::move(job));
    }
    if (batch->jobs.empty())
        return;
    if (!session_)
        session_ = std::make_unique<Session>();
    if (!session_->isOpen()) {
        session_.reset();
        emit failed(tr("Tessel could not create its recovery directory in %1.")
                        .arg(QDir::toNativeSeparators(root())));
        return;
    }
    pending_ = batch;
    Session* session = session_.get();
    // The worker only touches the batch it was handed and the session, which
    // the main thread leaves alone until the batch is applied.
    worker_ = std::thread([this, batch, session] {
        for (Job& job : batch->jobs) {
            QString error;
            job.ok = session->write(job.key, job.state, job.originalPath, job.title, &error);
            if (!job.ok && batch->error.isEmpty())
                batch->error = error;
            job.state = DocumentState(); // the pixels are on disk, let them go
        }
        QMetaObject::invokeMethod(
            this, [this, batch] { batchFinished(batch); }, Qt::QueuedConnection);
    });
}

void Autosaver::batchFinished(std::shared_ptr<Batch> batch)
{
    if (!batch || batch->applied)
        return;
    batch->applied = true;
    if (worker_.joinable())
        worker_.join();
    int count = 0;
    for (const Job& job : batch->jobs) {
        if (!job.ok)
            continue;
        written_[job.key] = job.revision;
        ++count;
    }
    if (pending_ == batch)
        pending_.reset();
    if (!batch->error.isEmpty())
        emit failed(batch->error);
    if (count > 0)
        emit wrote(count);
}

void Autosaver::waitForIdle()
{
    if (worker_.joinable())
        worker_.join();
    // The completion hop is queued, and callers that are about to delete files
    // need the bookkeeping now rather than at the next turn of the event loop.
    if (pending_)
        batchFinished(pending_);
}

void Autosaver::forget(int key)
{
    waitForIdle();
    written_.erase(key);
    if (session_)
        session_->drop(key);
}

void Autosaver::discardAll()
{
    waitForIdle();
    written_.clear();
    if (!session_)
        return;
    session_->dropAll();
    session_.reset(); // removes the now empty directory
}

} // namespace tessel::recovery
