#pragma once

/// The settings store: what pages exist, what the values are, and where they persist.
///
/// # Extensible, in the one way that matters
///
/// Pages can be added at any time by anything — the application at startup, a plugin at load, a
/// module that only exists in some builds. `addPage` is the whole extension surface, and it is
/// deliberately not a virtual interface: a page is DATA (see `SettingsModel.h`), so a contributor
/// needs no inheritance, no vtable and no link-time dependency on this class beyond the struct.
///
/// # Why the store owns values rather than each contributor
///
/// A plugin that kept its own settings would have to persist them itself, choose a format, and
/// handle a user who uninstalls and reinstalls it. Worse, the settings window could not show a
/// page for a plugin that is not currently loaded — so a user could not turn a plugin's ribbon
/// contribution back on without the plugin being on, which is the wrong way round.
///
/// So the store owns every value under one `QSettings`, keyed by the setting's id. An uninstalled
/// contributor's values stay put, and reinstalling restores them. This is the same reasoning as
/// PLUGIN_CONTRACT §4A: the host stores a plugin's data so the data can outlive the plugin.
///
/// # Nothing here is domain-specific
///
/// The store knows about pages, groups, ids and QVariants. It does not know what a viewport, a
/// document or a part is. An application whose documents are city blocks uses it unchanged.

#include "proshell/SettingsModel.h"

#include <QObject>
#include <QString>
#include <QVariant>

#include <vector>

class QSettings;

namespace proshell {

class Settings : public QObject {
    Q_OBJECT
public:
    /// `organisation` and `application` are the QSettings scope, so two applications built on this
    /// shell do not share a preferences file. Passed in rather than read from QCoreApplication
    /// because a test must be able to use a scope it can throw away.
    Settings(QString organisation, QString application, QObject* parent = nullptr);
    ~Settings() override;

    /// Adds a page, or MERGES into an existing one when the id already exists.
    ///
    /// Merging rather than refusing, unlike the ribbon's tabs: two contributors adding groups to a
    /// shared "General" page is the normal case, not a conflict. A duplicate SETTING id inside the
    /// merge is refused though — that one really is a collision, and silently keeping one of two
    /// would make which value a user edited depend on load order.
    void addPage(SettingsPage page);

    [[nodiscard]] const std::vector<SettingsPage>& pages() const noexcept { return pages_; }

    /// The current value, or the setting's fallback, or an invalid QVariant if the id is unknown.
    ///
    /// Unknown returns invalid rather than a default-constructed value of some assumed type: a
    /// caller that typos an id should see nothing rather than silently get `false`.
    [[nodiscard]] QVariant value(const QString& id) const;

    /// Typed convenience. `orFallback` is returned when the id is unknown, so a caller reading a
    /// setting a plugin has not registered gets its own sensible answer rather than zero.
    [[nodiscard]] bool boolean(const QString& id, bool orFallback = false) const;
    [[nodiscard]] int integer(const QString& id, int orFallback = 0) const;
    [[nodiscard]] double real(const QString& id, double orFallback = 0.0) const;
    [[nodiscard]] QString text(const QString& id, const QString& orFallback = {}) const;

    /// Sets and persists. No-op when the value is unchanged, so a dialog that writes every field on
    /// close does not emit a storm of changes nothing acted on.
    void setValue(const QString& id, const QVariant& value);

    /// Back to the declared fallback, and REMOVED from storage rather than written as the default.
    /// The difference matters on the next release: a value stored explicitly survives a change to
    /// the default, and a user who never overrode it should follow the new default instead.
    void reset(const QString& id);
    void resetPage(const QString& pageId);

    /// Whether the user has overridden this, which is what a dialog uses to show a reset affordance
    /// only where it would do something.
    [[nodiscard]] bool isOverridden(const QString& id) const;

    [[nodiscard]] const Setting* find(const QString& id) const;

signals:
    /// One id at a time. A coarse "something changed" would make every listener re-read everything,
    /// which for a setting that drives a rebuild is the difference between instant and visible.
    void changed(const QString& id, const QVariant& value);

private:
    std::vector<SettingsPage> pages_;
    QSettings* store_ = nullptr;
};

}  // namespace proshell
