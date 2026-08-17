#pragma once

/// What an application's settings look like, as data.
///
/// # Why data and not widgets
///
/// The same argument as `HomeModel`, and one the plugin contract already makes for a different
/// reason: an application that hands over widgets can only ever have one front end. This shell is
/// already meant to serve a second application, and a third front end — touch — is on the roadmap.
/// A settings page described as DATA renders as a desktop dialog here, as a grouped list on a
/// tablet, and as a text dump in a support bundle, from one declaration.
///
/// It also makes settings extensible by things that cannot link this library at all. A plugin
/// declares a `Setting`; it does not construct a `QCheckBox`.
///
/// # What varies between applications, and what does not
///
/// The ARRANGEMENT does not vary: a settings window is a page list beside a scrolling form of
/// labelled fields, with a description under each and a reset. That is settled interaction, and
/// every CAD application in production uses it. What varies is which pages exist and which fields
/// are on them — which is exactly the data below and nothing else.
///
/// # Ids are permanent; labels are free
///
/// A setting's `id` is written into the user's stored preferences, so renaming it silently discards
/// whatever they had chosen. The `label` is what they read and may be reworded or translated at
/// will. This is the same rule as a sketch parameter's name, a ribbon section's id, and a plugin
/// feature's type name — the third time it has come up, so it is worth stating as a rule rather
/// than rediscovering: **anything persisted is permanent, anything displayed is not.**

#include <QString>
#include <QStringList>
#include <QVariant>

#include <vector>

namespace proshell {

/// What kind of value a setting holds, which is also how it is rendered.
///
/// Deliberately small. Every addition is a widget the dialog must know how to draw and a type the
/// store must know how to persist, so a kind earns its place by being needed rather than by being
/// conceivable. A colour picker and a keyboard shortcut are the two most likely additions.
enum class SettingKind {
    Bool,     ///< a checkbox
    Int,      ///< a spin box, bounded by minimum/maximum
    Double,   ///< a spin box with decimals
    Text,     ///< a single-line edit
    Choice,   ///< a combo box over `choices`
};

/// One user-changeable value.
struct Setting {
    /// Stable, dotted, and PERSISTED — "viewport.navigation". Never renamed: a rename silently
    /// resets whatever the user had chosen, which is worse than the wording being imperfect.
    QString id;
    QString label;

    /// One sentence under the field, saying what it affects. Not a tooltip: a setting whose effect
    /// is only discoverable by trying it is a setting most users leave alone.
    QString description;

    SettingKind kind = SettingKind::Bool;

    /// The value the application should use if the user has never touched this. Kept separately
    /// from the current value so "reset" is possible at all, and so a default can CHANGE between
    /// releases for users who never overrode it.
    QVariant fallback;

    /// Int and Double only. A bound the dialog enforces, so an application does not have to
    /// validate what the widget could have prevented.
    double minimum = 0.0;
    double maximum = 0.0;

    /// Choice only. The stored value is the INDEX, not the text, so a label can be translated
    /// without invalidating what the user picked.
    QStringList choices;
};

/// A labelled run of settings within a page. Purely visual grouping — a group has no id because
/// nothing is stored against it.
struct SettingsGroup {
    QString label;
    std::vector<Setting> settings;
};

/// One entry in the settings window's page list.
struct SettingsPage {
    /// Stable, because a window that reopens on the page you were last looking at has to name it.
    QString id;
    QString label;
    QString iconName;   ///< resolved by the shell's theme, so it matches a dark theme it never saw
    std::vector<SettingsGroup> groups;
};

}  // namespace proshell
