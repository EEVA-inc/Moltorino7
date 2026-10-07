#include "providers/twitch/ModerationActionLogs.hpp"

#include <QPointer>
#include <QTimer>

#include <algorithm>
#include <utility>

namespace chatterino {

namespace {

void addKind(ModerationActionLogCounts &counts, GqlModerationActionKind kind)
{
    switch (kind)
    {
        case GqlModerationActionKind::Ban:
            counts.bans++;
            break;
        case GqlModerationActionKind::Timeout:
            counts.timeouts++;
            break;
        case GqlModerationActionKind::Unban:
            counts.unbans++;
            break;
        case GqlModerationActionKind::Untimeout:
            counts.untimeouts++;
            break;
        case GqlModerationActionKind::Delete:
            counts.deletes++;
            break;
        case GqlModerationActionKind::Other:
            counts.other++;
            break;
        case GqlModerationActionKind::Message:
            break;
    }
}

bool isShownModerationAction(GqlModerationActionKind kind)
{
    switch (kind)
    {
        case GqlModerationActionKind::Ban:
        case GqlModerationActionKind::Timeout:
        case GqlModerationActionKind::Unban:
        case GqlModerationActionKind::Untimeout:
        case GqlModerationActionKind::Delete:
        case GqlModerationActionKind::Other:
            return true;
        case GqlModerationActionKind::Message:
            return false;
    }
    return false;
}

QString normalized(QString value)
{
    return value.trimmed().toLower();
}

}  // namespace

int ModerationActionLogCounts::countedTotal() const
{
    return this->bans + this->timeouts;
}

int ModerationActionLogCounts::rawTotal() const
{
    return this->countedTotal() + this->deletes + this->unbans +
           this->untimeouts + this->other;
}

QString moderationActionKindText(GqlModerationActionKind kind)
{
    switch (kind)
    {
        case GqlModerationActionKind::Ban:
            return "ban";
        case GqlModerationActionKind::Unban:
            return "unban";
        case GqlModerationActionKind::Timeout:
            return "timeout";
        case GqlModerationActionKind::Untimeout:
            return "untimeout";
        case GqlModerationActionKind::Delete:
            return "delete";
        case GqlModerationActionKind::Message:
            return "message";
        case GqlModerationActionKind::Other:
            return "other";
    }
    return "other";
}

bool moderationActionKindCounts(GqlModerationActionKind kind)
{
    return kind == GqlModerationActionKind::Ban ||
           kind == GqlModerationActionKind::Timeout;
}

ModerationActionLogScanner::ModerationActionLogScanner(
    ModerationActionLogScanRequest request, QObject *parent)
    : QObject(parent)
    , request_(std::move(request))
{
    this->request_.maxPages = std::clamp(this->request_.maxPages, 1, 10000);
    this->request_.maxRetainedEvents =
        std::clamp(this->request_.maxRetainedEvents, 0, 20000);
    if (this->request_.pageDelayMs < 0)
    {
        this->request_.pageDelayMs = 0;
    }
}

void ModerationActionLogScanner::start()
{
    if (this->running_)
    {
        return;
    }

    this->running_ = true;
    this->cancelled_ = false;
    this->fetchNext();
}

void ModerationActionLogScanner::cancel()
{
    this->cancelled_ = true;
}

ModerationActionLogScanSnapshot ModerationActionLogScanner::snapshot() const
{
    auto snapshot = this->snapshot_;
    snapshot.moderators.reserve(this->moderators_.size());
    for (const auto &entry : this->moderators_)
    {
        snapshot.moderators.push_back(entry.summary);
    }
    std::sort(snapshot.moderators.begin(), snapshot.moderators.end(),
              [](const auto &a, const auto &b) {
                  const auto aTotal = a.counts.countedTotal();
                  const auto bTotal = b.counts.countedTotal();
                  if (aTotal != bTotal)
                  {
                      return aTotal > bTotal;
                  }
                  const auto aName =
                      a.login.isEmpty() ? a.displayName : a.login;
                  const auto bName =
                      b.login.isEmpty() ? b.displayName : b.login;
                  return aName.compare(bName, Qt::CaseInsensitive) < 0;
              });
    return snapshot;
}

void ModerationActionLogScanner::fetchNext()
{
    if (!this->running_ || this->cancelled_)
    {
        this->finish();
        return;
    }
    if (this->snapshot_.pagesRead >= this->request_.maxPages)
    {
        this->finish(true);
        return;
    }

    const QPointer<ModerationActionLogScanner> self(this);
    TwitchGql::getModerationActionLogs(
        this->request_.channelId, this->cursor_, this->request_.oauthToken,
        [self](GqlModerationActionLogPage page) {
            if (!self)
            {
                return;
            }
            self->processPage(page);
        },
        [self](const QString &error) {
            if (!self)
            {
                return;
            }
            self->fail(error);
        });
}

void ModerationActionLogScanner::processPage(
    const GqlModerationActionLogPage &page)
{
    if (!this->running_)
    {
        return;
    }

    this->snapshot_.pagesRead++;
    this->snapshot_.rawActionsSeen += page.actions.size();

    bool stopForCutoff = false;
    for (const auto &action : page.actions)
    {
        if (action.createdAt.isValid())
        {
            this->snapshot_.oldestSeen = action.createdAt;
            if (this->request_.cutoffUtc.isValid() &&
                action.createdAt < this->request_.cutoffUtc)
            {
                stopForCutoff = true;
                break;
            }
        }
        this->processAction(action);
    }

    this->snapshot_.lastCursor = page.nextCursor;
    this->emitProgress();

    if (this->cancelled_)
    {
        this->finish();
        return;
    }
    if (stopForCutoff)
    {
        this->snapshot_.reachedCutoff = true;
        this->finish();
        return;
    }
    if (!page.hasNextPage || page.nextCursor.isEmpty())
    {
        this->finish();
        return;
    }
    if (!this->cursor_.isEmpty() && page.nextCursor == this->cursor_)
    {
        this->finish(true);
        return;
    }

    this->cursor_ = page.nextCursor;
    QTimer::singleShot(this->request_.pageDelayMs, this, [this] {
        this->fetchNext();
    });
}

void ModerationActionLogScanner::processAction(
    const GqlModerationActionLogEntry &action)
{
    if (!this->matchesModeratorFilter(action))
    {
        return;
    }
    if (!isShownModerationAction(action.kind))
    {
        return;
    }

    addKind(this->snapshot_.totals, action.kind);

    if (this->snapshot_.events.size() < this->request_.maxRetainedEvents)
    {
        this->snapshot_.events.push_back({
            .id = action.id,
            .kind = action.kind,
            .createdAt = action.createdAt,
            .moderatorId = action.moderatorId,
            .moderatorLogin = action.moderatorLogin,
            .moderatorDisplayName = action.moderatorDisplayName,
            .targetId = action.targetId,
            .targetLogin = action.targetLogin,
            .targetDisplayName = action.targetDisplayName,
            .text = action.text.left(2000),
        });
    }
    else
    {
        this->snapshot_.eventsTruncated = true;
    }

    const auto key = this->moderatorKey(action);
    auto &entry = this->moderators_[key];
    if (entry.summary.id.isEmpty())
    {
        entry.summary.id = action.moderatorId;
    }

    const auto login = action.moderatorLogin.trimmed();
    if (!login.isEmpty() && entry.summary.login.isEmpty())
    {
        entry.summary.login = login;
    }

    const auto displayName = action.moderatorDisplayName.trimmed();
    if (!displayName.isEmpty() && entry.summary.displayName.isEmpty())
    {
        entry.summary.displayName = displayName;
    }
    if (entry.summary.displayName.isEmpty())
    {
        entry.summary.displayName =
            entry.summary.login.isEmpty() ? QStringLiteral("Unknown")
                                          : entry.summary.login;
    }
    addKind(entry.summary.counts, action.kind);
}

void ModerationActionLogScanner::emitProgress()
{
    if (this->onProgress)
    {
        this->onProgress(this->snapshot());
    }
}

void ModerationActionLogScanner::finish(bool truncated)
{
    if (!this->running_)
    {
        return;
    }
    this->running_ = false;
    this->snapshot_.cancelled = this->cancelled_;
    this->snapshot_.truncated = truncated;
    this->snapshot_.complete = true;
    if (this->onDone)
    {
        this->onDone(this->snapshot());
    }
}

void ModerationActionLogScanner::fail(const QString &message)
{
    if (!this->running_)
    {
        return;
    }
    this->running_ = false;
    if (this->onError)
    {
        this->onError(message);
    }
}

bool ModerationActionLogScanner::matchesModeratorFilter(
    const GqlModerationActionLogEntry &action) const
{
    if (this->request_.moderatorId.trimmed().isEmpty() &&
        this->request_.moderatorLogin.trimmed().isEmpty())
    {
        return true;
    }

    const auto wantedId = this->request_.moderatorId.trimmed();
    if (!wantedId.isEmpty() && action.moderatorId == wantedId)
    {
        return true;
    }

    const auto wantedLogin = normalized(this->request_.moderatorLogin);
    if (wantedLogin.isEmpty())
    {
        return false;
    }

    return normalized(action.moderatorLogin) == wantedLogin ||
           normalized(action.moderatorDisplayName) == wantedLogin;
}

QString ModerationActionLogScanner::moderatorKey(
    const GqlModerationActionLogEntry &action) const
{
    if (!action.moderatorId.isEmpty())
    {
        return "id:" + action.moderatorId;
    }
    if (!action.moderatorLogin.isEmpty())
    {
        return "login:" + normalized(action.moderatorLogin);
    }
    if (!action.moderatorDisplayName.isEmpty())
    {
        return "display:" + normalized(action.moderatorDisplayName);
    }
    return "unknown";
}

}  // namespace chatterino
