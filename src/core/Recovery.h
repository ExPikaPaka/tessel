#pragma once

#include <functional>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

#include "core/Document.h"

class QLockFile;
class QSettings;

namespace tessel::recovery {

// Recovery copies live under QStandardPaths::AppDataLocation, one directory
// per run of the program. Every directory carries a lock file, which is how a
// later run tells a session that crashed from one that is still going: the
// lock of a dead process can be taken over, the lock of a live one cannot.
QString root();
// Points the whole module somewhere else, so the tests never write into the
// real user data directory.
void setRoot(const QString& directory);

// ---------------------------------------------------------------- options

constexpr int kMinAutosaveMinutes = 1;
constexpr int kMaxAutosaveMinutes = 120;
constexpr int kMinKeepDays = 1;
constexpr int kMaxKeepDays = 365;

struct Options {
    bool autosave = true;
    int autosaveMinutes = 2;
    bool restoreSession = true;
    // How long an abandoned recovery file is kept before it is deleted
    // unasked. Without this the directory grows for the lifetime of the
    // installation, because "decide later" is a valid answer.
    int keepDays = 14;
};

Options loadOptions(const QSettings& settings);
void storeOptions(QSettings& settings, const Options& options);

// ---------------------------------------------------------------- session list

// The documents that were open last time, for the reopen offer. Only
// documents with a path on disk are worth remembering: an unnamed one has
// nothing to reopen from, and is the recovery case instead.
struct OpenDocuments {
    QStringList paths;
    int current = -1;
};

OpenDocuments loadOpenDocuments(const QSettings& settings);
void storeOpenDocuments(QSettings& settings, const OpenDocuments& documents);
// Drops the paths that have gone away since, keeping `current` on the same
// document where it survived.
OpenDocuments existingOnly(const OpenDocuments& documents);

// ---------------------------------------------------------------- entries

// One recovery copy found on disk.
struct Entry {
    QString path;          // the recovery copy itself, a .tessel file
    QString originalPath;  // where the document came from, empty if never saved
    QString title;
    qint64 size = 0;
    QDateTime savedAt;

    QString displayName() const;
};

// One session that is not running any more, with everything it left behind.
struct Abandoned {
    QString directory;
    std::vector<Entry> entries;
    // The claim on the dead session's lock. Held for as long as the caller
    // keeps this around, so a second instance started in the meantime is not
    // offered the same files at the same time.
    std::shared_ptr<QLockFile> claim;
};

// Newest first, so the dialog lists the most recent work at the top.
std::vector<Abandoned> findAbandoned();
std::vector<Entry> allEntries(const std::vector<Abandoned>& sessions);
// Deletes the files and releases the claims. The vector is emptied.
void discard(std::vector<Abandoned>& sessions);
// Deletes abandoned sessions whose newest file is older than `keepDays`.
// Returns how many directories were removed.
int pruneOlderThan(int keepDays, const QDateTime& now = QDateTime::currentDateTime());

// ---------------------------------------------------------------- session

// The locked directory this run writes its recovery copies into. Created on
// construction and, when nothing is left to recover, removed again on
// destruction, so a run that ends cleanly leaves no trace.
class Session {
public:
    Session();
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    bool isOpen() const { return lock_ != nullptr; }
    QString directory() const { return directory_; }

    // Blocking: this encodes and writes a whole image. The autosaver calls it
    // on a worker thread for that reason.
    bool write(int key, const DocumentState& state, const QString& originalPath,
               const QString& title, QString* error);
    // The document was saved or closed cleanly, so there is nothing to recover.
    void drop(int key);
    void dropAll();

    QString pathFor(int key) const;

private:
    QString directory_;
    std::unique_ptr<QLockFile> lock_;
};

// ---------------------------------------------------------------- autosaver

// Writes a recovery copy of every changed document on a timer, on a worker
// thread so the interface never waits for the disk.
//
// The owner supplies a source callback instead of a list of documents: the
// autosaver asks for the open documents when a write is due, which keeps it
// out of the business of tracking windows, and the snapshots are only taken
// for the documents whose revision moved since the last write.
class Autosaver : public QObject {
    Q_OBJECT

public:
    struct Request {
        int key = 0;
        QString originalPath;
        QString title;
        // Anything that changes when the document changes. The window derives
        // it from the undo history.
        qint64 revision = 0;
        std::function<DocumentState()> snapshot;
    };
    using Source = std::function<std::vector<Request>()>;

    explicit Autosaver(QObject* parent = nullptr);
    ~Autosaver() override;

    void setSource(Source source);
    // Minutes between writes. The timer is only running while the autosaver
    // is enabled and the interval is sane.
    void setIntervalMinutes(int minutes);
    int intervalMinutes() const { return minutes_; }
    void setEnabled(bool enabled);
    bool isEnabled() const { return enabled_; }
    // Milliseconds until the next write, or -1 when the timer is off.
    int millisecondsToNextWrite() const;

    // Takes the snapshots that are due and hands them to the worker. The
    // timer calls this, and so do the tests.
    void saveNow();
    bool isBusy() const { return pending_ != nullptr; }
    // Waits for the worker and applies its bookkeeping, as the destructor and
    // the drop calls below have to do before they touch the session.
    void waitForIdle();

    void forget(int key);
    // Everything was saved or closed cleanly: drop the files and the directory.
    void discardAll();

    const Session* session() const { return session_.get(); }

signals:
    void wrote(int count);
    void failed(const QString& message);

private:
    struct Job {
        int key = 0;
        qint64 revision = 0;
        QString originalPath;
        QString title;
        DocumentState state;
        bool ok = false;
    };
    struct Batch {
        std::vector<Job> jobs;
        QString error;
        bool applied = false;
    };

    // By value: applying a batch clears `pending_`, which would leave a
    // reference to it dangling.
    void batchFinished(std::shared_ptr<Batch> batch);
    void restartTimer();

    Source source_;
    QTimer timer_;
    int minutes_ = 2;
    bool enabled_ = true;
    std::unique_ptr<Session> session_;
    std::unordered_map<int, qint64> written_;
    std::shared_ptr<Batch> pending_;
    std::thread worker_;
};

} // namespace tessel::recovery
