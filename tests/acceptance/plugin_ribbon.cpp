// The three things a plugin may add to the ribbon, in Fusion's vocabulary.
//
//   TAB      — "SOLID", "SHEET METAL". The largest thing, and the one to add least often.
//   SECTION  — "CREATE", "MODIFY" within a tab.
//   COMMAND  — "Extrude", "Box" within a section.
//
// All three are legal, which is a deliberate reversal of PLUGIN_CONTRACT 7.3's first draft. That
// forbade new tabs to avoid the failure that disfigures every mature CAD ribbon — Revit had to
// retrofit a one-tab limit per add-in — and the reason to allow them anyway is that a real domain
// suite genuinely owns a tab. The protection moved from "you may not" to "the user may hide it".
//
// What these tests are really about is the ORDERED VALIDATION. A section must name a tab that
// exists and a command must name a section that exists, and naming something unknown is refused
// rather than created. Implicit creation means a typo produces an empty tab called "Creat" and the
// plugin author sees a missing button with no error anywhere.

#include "cad/abi/cad_plugin_abi.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>

namespace {

class Host {
public:
    Host() : session_(cad_session_create()) { host_ = cad_plugin_host(session_); }
    ~Host() { cad_session_release(session_); }
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;
    [[nodiscard]] const CadHost* h() const { return host_; }
    [[nodiscard]] void* ctx() const { return host_->host_ctx; }
    [[nodiscard]] CadSession session() const { return session_; }

    [[nodiscard]] uint32_t commands() const {
        uint32_t n = 0;
        cad_ribbon_counts(session_, nullptr, nullptr, &n);
        return n;
    }
    [[nodiscard]] uint32_t tabs() const {
        uint32_t n = 0;
        cad_ribbon_counts(session_, &n, nullptr, nullptr);
        return n;
    }
    [[nodiscard]] uint32_t sections() const {
        uint32_t n = 0;
        cad_ribbon_counts(session_, nullptr, &n, nullptr);
        return n;
    }

private:
    CadSession session_ = 0;
    const CadHost* host_ = nullptr;
};

CadStatus doNothing(void*, CadCommandCtx) { return CAD_OK; }

CadCommandDesc command(const char* id, const char* section) {
    CadCommandDesc d{};
    d.struct_size = sizeof(CadCommandDesc);
    d.struct_version = 1;
    d.id = id;
    d.label = "Demo";
    d.section = section;
    d.tab = CAD_TAB_MODEL;
    d.placement = CAD_UI_RIBBON;
    d.invoke = &doNothing;
    return d;
}

}  // namespace

TEST_CASE("the built-in ribbon is visible to a plugin", "[plugin][ribbon]") {
    // A plugin has to be able to name CAD_SECTION_CREATE and be validated against something. These
    // ids are in the header, not invented in the host, because a plugin naming one must still land
    // there in ten years — and a string literal in an implementation is not a promise.
    Host host;
    CHECK(host.tabs() >= 6);
    CHECK(host.sections() >= 7);
    CHECK(host.commands() == 0);   // built-in COMMANDS are the shell's, not the ABI's
}

TEST_CASE("a plugin adds a button to an existing section", "[plugin][ribbon]") {
    // The common case, and the one the user asked for first: extend Create rather than build a
    // parallel ribbon next to it.
    Host host;
    const auto desc = command("com.acme.demo.pad", CAD_SECTION_CREATE);
    REQUIRE(host.h()->register_command != nullptr);
    CHECK(host.h()->register_command(host.ctx(), &desc) == CAD_OK);
    CHECK(host.commands() == 1);
    CHECK(host.tabs() >= 6);       // and it did NOT invent a tab to hold it
}

TEST_CASE("a plugin adds a section to an existing tab", "[plugin][ribbon]") {
    Host host;
    const uint32_t before = host.sections();

    CadSectionDesc section{};
    section.struct_size = sizeof(CadSectionDesc);
    section.struct_version = 1;
    section.id = "com.acme.sheetmetal.flanges";
    section.label = "FLANGES";
    section.tab = CAD_TAB_MODEL;
    REQUIRE(host.h()->register_section != nullptr);
    CHECK(host.h()->register_section(host.ctx(), &section) == CAD_OK);
    CHECK(host.sections() == before + 1);

    const auto desc = command("com.acme.sheetmetal.flange", "com.acme.sheetmetal.flanges");
    CHECK(host.h()->register_command(host.ctx(), &desc) == CAD_OK);
}

TEST_CASE("a plugin owns a whole new tab", "[plugin][ribbon]") {
    Host host;
    const uint32_t before = host.tabs();

    CadTabDesc tab{};
    tab.struct_size = sizeof(CadTabDesc);
    tab.struct_version = 1;
    tab.id = "com.acme.sheetmetal.tab";
    tab.label = "SHEET METAL";
    tab.order = 1000;
    REQUIRE(host.h()->register_tab != nullptr);
    CHECK(host.h()->register_tab(host.ctx(), &tab) == CAD_OK);
    CHECK(host.tabs() == before + 1);

    CadSectionDesc section{};
    section.struct_size = sizeof(CadSectionDesc);
    section.struct_version = 1;
    section.id = "com.acme.sheetmetal.create";
    section.label = "CREATE";
    section.tab = "com.acme.sheetmetal.tab";
    CHECK(host.h()->register_section(host.ctx(), &section) == CAD_OK);

    const auto desc = command("com.acme.sheetmetal.base", "com.acme.sheetmetal.create");
    CHECK(host.h()->register_command(host.ctx(), &desc) == CAD_OK);
}

TEST_CASE("a section naming a tab that does not exist is refused", "[plugin][ribbon]") {
    // The whole point. Creating the tab implicitly would mean a typo produces an empty tab and the
    // author sees a missing button with nothing to explain it.
    Host host;
    const uint32_t before = host.sections();

    CadSectionDesc section{};
    section.struct_size = sizeof(CadSectionDesc);
    section.struct_version = 1;
    section.id = "com.acme.orphan";
    section.label = "ORPHAN";
    section.tab = "com.acme.tab.that.was.never.registered";
    CHECK(host.h()->register_section(host.ctx(), &section) == CAD_ERR_INVALID_INPUT);
    CHECK(host.sections() == before);

    const CadStr why = host.h()->last_error(host.ctx());
    REQUIRE(why.len > 0);
    // The message must NAME the missing tab, or an author with six sections cannot tell which.
    CHECK(std::strstr(why.data, "never.registered") != nullptr);
}

TEST_CASE("a command naming a section that does not exist is refused", "[plugin][ribbon]") {
    Host host;
    const auto desc = command("com.acme.lost", "com.acme.section.that.does.not.exist");
    CHECK(host.h()->register_command(host.ctx(), &desc) == CAD_ERR_INVALID_INPUT);
    CHECK(host.commands() == 0);
}

TEST_CASE("two plugins cannot claim one id", "[plugin][ribbon]") {
    // Refused rather than merged, for tabs, sections and commands alike. Sharing an id would mean
    // each plugin believed it owned the thing, and which one a user saw would depend on load order
    // — the same reason a duplicate feature type is refused.
    Host host;
    CadTabDesc tab{};
    tab.struct_size = sizeof(CadTabDesc);
    tab.struct_version = 1;
    tab.id = "com.acme.tab";
    tab.label = "ACME";
    CHECK(host.h()->register_tab(host.ctx(), &tab) == CAD_OK);
    CHECK(host.h()->register_tab(host.ctx(), &tab) == CAD_ERR_INVALID_INPUT);

    const auto desc = command("com.acme.twice", CAD_SECTION_CREATE);
    CHECK(host.h()->register_command(host.ctx(), &desc) == CAD_OK);
    CHECK(host.h()->register_command(host.ctx(), &desc) == CAD_ERR_INVALID_INPUT);
}

TEST_CASE("a command from an older header still lands somewhere", "[plugin][ribbon]") {
    // A plugin built before placement existed has no tab or section field. It must not vanish: a
    // command that registers successfully and appears nowhere is worse than one that is refused.
    Host host;
    CadCommandDesc old{};
    old.struct_size = offsetof(CadCommandDesc, tab);   // ends before the placement fields
    old.struct_version = 1;
    old.id = "com.acme.legacy";
    old.label = "Legacy";
    old.invoke = &doNothing;

    CHECK(host.h()->register_command(host.ctx(), &old) == CAD_OK);
    CHECK(host.commands() == 1);
}

// ── plugin settings pages ───────────────────────────────────────────────────────────────
//
// A plugin DECLARES settings; it does not draw them. That is what lets one declaration become a
// desktop dialog, a tablet list and a line in a support bundle — and what stops fifteen plugins
// each inventing their own idea of what a preferences page looks like.
//
// The host stores the values, so an uninstalled plugin's settings survive and a user can re-enable
// a plugin's ribbon contribution without the plugin already being loaded. Same reasoning as §4A.

namespace {

CadSettingDesc boolSetting(const char* id, const char* label) {
    CadSettingDesc d{};
    d.struct_size = sizeof(CadSettingDesc);
    d.struct_version = 1;
    d.id = id;
    d.label = label;
    d.description = "Does a thing.";
    d.kind = CAD_SETTING_BOOL;
    d.default_value = 1.0;
    return d;
}

CadSettingsPageDesc pageDesc(const char* id, const char* label, const char* group) {
    CadSettingsPageDesc p{};
    p.struct_size = sizeof(CadSettingsPageDesc);
    p.struct_version = 1;
    p.id = id;
    p.label = label;
    p.group_label = group;
    return p;
}

}  // namespace

TEST_CASE("a plugin declares a settings page and it reads back", "[plugin][settings]") {
    Host host;
    REQUIRE(host.h()->register_settings_page != nullptr);

    const auto page = pageDesc("com.acme.sheetmetal.settings", "Sheet Metal", "Defaults");
    const CadSettingDesc settings[]{boolSetting("com.acme.sheetmetal.autoRelief", "Auto relief"),
                                    boolSetting("com.acme.sheetmetal.showFlatPattern", "Flat pattern")};
    CHECK(host.h()->register_settings_page(host.ctx(), &page, settings, 2) == CAD_OK);

    uint32_t pages = 0;
    REQUIRE(cad_settings_page_count(host.session(), &pages) == CAD_OK);
    REQUIRE(pages == 1);

    CadSettingsPageDesc out{};
    out.struct_size = sizeof(out);
    uint32_t count = 0;
    REQUIRE(cad_settings_page_at(host.session(), 0, &out, &count) == CAD_OK);
    CHECK(std::strcmp(out.label, "Sheet Metal") == 0);
    CHECK(std::strcmp(out.group_label, "Defaults") == 0);
    REQUIRE(count == 2);

    CadSettingDesc first{};
    first.struct_size = sizeof(first);
    REQUIRE(cad_settings_at(host.session(), 0, 0, &first) == CAD_OK);
    CHECK(std::strcmp(first.id, "com.acme.sheetmetal.autoRelief") == 0);
    CHECK(first.kind == CAD_SETTING_BOOL);
    CHECK(first.default_value == 1.0);
}

TEST_CASE("two plugins merge into one settings page", "[plugin][settings]") {
    // Merged, unlike a ribbon tab which is refused. Two plugins adding a group to a shared
    // "General" page is the normal case; two claiming one ribbon tab is not.
    Host host;
    const auto a = pageDesc("shared.general", "General", "Acme");
    const CadSettingDesc one[]{boolSetting("acme.thing", "Thing")};
    CHECK(host.h()->register_settings_page(host.ctx(), &a, one, 1) == CAD_OK);

    const auto b = pageDesc("shared.general", "General", "Other");
    const CadSettingDesc two[]{boolSetting("other.thing", "Thing")};
    CHECK(host.h()->register_settings_page(host.ctx(), &b, two, 1) == CAD_OK);

    uint32_t pages = 0;
    cad_settings_page_count(host.session(), &pages);
    CHECK(pages == 1);

    CadSettingsPageDesc out{};
    out.struct_size = sizeof(out);
    uint32_t count = 0;
    cad_settings_page_at(host.session(), 0, &out, &count);
    CHECK(count == 2);
}

TEST_CASE("a duplicate setting id is dropped, not shadowed", "[plugin][settings]") {
    Host host;
    const auto page = pageDesc("dup.page", "Dup", "Group");
    const CadSettingDesc settings[]{boolSetting("dup.id", "First"), boolSetting("dup.id", "Second")};
    CHECK(host.h()->register_settings_page(host.ctx(), &page, settings, 2) == CAD_OK);

    CadSettingsPageDesc out{};
    out.struct_size = sizeof(out);
    uint32_t count = 0;
    cad_settings_page_at(host.session(), 0, &out, &count);
    // One, not two. Which value a user edited must not depend on load order.
    CHECK(count == 1);
    CadSettingDesc kept{};
    kept.struct_size = sizeof(kept);
    cad_settings_at(host.session(), 0, 0, &kept);
    CHECK(std::strcmp(kept.label, "First") == 0);
}

TEST_CASE("an array of OLDER descriptors is walked by the caller's stride",
          "[plugin][settings]") {
    // The one that would corrupt memory if it were wrong. A plugin built against an earlier header
    // has SHORTER descriptors, so stepping through its array by our sizeof would read the second
    // element from the middle of the first — and the ids would come back as garbage pointers.
    Host host;

    struct OldSetting {
        uint32_t struct_size;
        uint32_t struct_version;
        const char* id;
        const char* label;
    };
    static_assert(sizeof(OldSetting) < sizeof(CadSettingDesc), "the point of this test");

    const OldSetting old[]{
        {sizeof(OldSetting), 1, "old.first", "First"},
        {sizeof(OldSetting), 1, "old.second", "Second"},
    };

    const auto page = pageDesc("old.page", "Old", "Group");
    CHECK(host.h()->register_settings_page(
              host.ctx(), &page, reinterpret_cast<const CadSettingDesc*>(old), 2) == CAD_OK);

    CadSettingsPageDesc out{};
    out.struct_size = sizeof(out);
    uint32_t count = 0;
    cad_settings_page_at(host.session(), 0, &out, &count);
    REQUIRE(count == 2);

    CadSettingDesc second{};
    second.struct_size = sizeof(second);
    REQUIRE(cad_settings_at(host.session(), 0, 1, &second) == CAD_OK);
    CHECK(std::strcmp(second.id, "old.second") == 0);
}
