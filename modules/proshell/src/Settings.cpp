#include "proshell/Settings.h"

#include <QSettings>

#include <algorithm>

namespace proshell {
namespace {

/// Every setting on a page, flattened. Groups are visual only, so lookup does not care about them.
template <class Fn>
void forEachSetting(const std::vector<SettingsPage>& pages, Fn&& fn) {
    for (const SettingsPage& page : pages) {
        for (const SettingsGroup& group : page.groups) {
            for (const Setting& setting : group.settings) {
                if (fn(page, setting)) return;
            }
        }
    }
}

}  // namespace

Settings::Settings(QString organisation, QString application, QObject* parent)
    : QObject(parent),
      store_(new QSettings(organisation, application, this)) {}

Settings::~Settings() = default;

void Settings::addPage(SettingsPage page) {
    const auto existing = std::find_if(pages_.begin(), pages_.end(),
                                       [&](const SettingsPage& p) { return p.id == page.id; });
    if (existing == pages_.end()) {
        pages_.push_back(std::move(page));
        return;
    }

    // Merge. Two contributors adding groups to a shared "General" page is the normal case; a
    // duplicate SETTING id inside it is not, and is dropped rather than shadowing the first —
    // silently keeping one of two would make which value a user edited depend on load order.
    for (SettingsGroup& incoming : page.groups) {
        incoming.settings.erase(
            std::remove_if(incoming.settings.begin(), incoming.settings.end(),
                           [&](const Setting& s) { return find(s.id) != nullptr; }),
            incoming.settings.end());
        if (!incoming.settings.empty()) existing->groups.push_back(std::move(incoming));
    }
}

const Setting* Settings::find(const QString& id) const {
    const Setting* found = nullptr;
    forEachSetting(pages_, [&](const SettingsPage&, const Setting& s) {
        if (s.id == id) {
            found = &s;
            return true;
        }
        return false;
    });
    return found;
}

QVariant Settings::value(const QString& id) const {
    const Setting* setting = find(id);
    // Unknown id returns INVALID rather than a default-constructed value: a caller that typos an id
    // should see nothing rather than silently get `false` and act on it.
    if (setting == nullptr) return {};
    return store_->value(id, setting->fallback);
}

bool Settings::boolean(const QString& id, bool orFallback) const {
    const QVariant v = value(id);
    return v.isValid() ? v.toBool() : orFallback;
}

int Settings::integer(const QString& id, int orFallback) const {
    const QVariant v = value(id);
    return v.isValid() ? v.toInt() : orFallback;
}

double Settings::real(const QString& id, double orFallback) const {
    const QVariant v = value(id);
    return v.isValid() ? v.toDouble() : orFallback;
}

QString Settings::text(const QString& id, const QString& orFallback) const {
    const QVariant v = value(id);
    return v.isValid() ? v.toString() : orFallback;
}

void Settings::setValue(const QString& id, const QVariant& next) {
    if (find(id) == nullptr) return;   // refuse to store a value nothing declared
    if (value(id) == next) return;     // a dialog writing every field on close must not storm
    store_->setValue(id, next);
    emit changed(id, next);
}

void Settings::reset(const QString& id) {
    const Setting* setting = find(id);
    if (setting == nullptr || !store_->contains(id)) return;
    // REMOVED, not written as the default. A value stored explicitly survives a change to the
    // default in a later release; a user who never overrode it should follow the new default.
    store_->remove(id);
    emit changed(id, setting->fallback);
}

void Settings::resetPage(const QString& pageId) {
    const auto page = std::find_if(pages_.begin(), pages_.end(),
                                   [&](const SettingsPage& p) { return p.id == pageId; });
    if (page == pages_.end()) return;
    for (const SettingsGroup& group : page->groups) {
        for (const Setting& setting : group.settings) reset(setting.id);
    }
}

bool Settings::isOverridden(const QString& id) const { return store_->contains(id); }

}  // namespace proshell
