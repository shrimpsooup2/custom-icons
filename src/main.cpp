#include <Geode/Geode.hpp>
#include <Geode/modify/GJGarageLayer.hpp>
#include <Geode/modify/LoadingLayer.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/ScrollLayer.hpp>
#include <Geode/utils/file.hpp>
#include <Geode/utils/async.hpp>
#include <filesystem>
#include <regex>

using namespace geode::prelude;
namespace fs = std::filesystem;

// ---------- icon sheet naming ----------

static const char* PREFIXES[] = {
    "player", "ship", "player_ball", "bird", "dart",
    "robot", "spider", "swing", "jetpack"
};
static const char* TYPE_NAMES[] = {
    "Cube", "Ship", "Ball", "UFO", "Wave", "Robot", "Spider", "Swing", "Jetpack"
};

static int clampType(int t) { return (t < 0 || t > 8) ? 0 : t; }

static std::string sheetBase(IconType type, int id) {
    return fmt::format("{}_{:02}", PREFIXES[clampType(static_cast<int>(type))], id);
}

// ---------- storage ----------
// variations/<sheet>/<name>.png (+ <name>[-hd|-uhd].plist)  = imported skins
// active/(icons/)<sheet>[-hd|-uhd].png/.plist               = the applied skin,
// served to the game through a front-of-line resource search path (the same
// mechanism texture packs use). The plist always sits NEXT TO the png because
// cocos resolves a sheet's texture relative to the plist's own directory.

static fs::path activeDir() { return Mod::get()->getSaveDir() / "active"; }
static fs::path variationsDir(std::string const& base) {
    return Mod::get()->getSaveDir() / "variations" / base;
}

static std::string pathKey(std::string s) {
    for (auto& c : s) {
        if (c == '\\') c = '/';
    }
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

// register our override dir with Geode's priority-path list. Geode rebuilds
// cocos' search paths from its own internal lists whenever packs/resources
// change, so mutating setSearchPaths directly gets clobbered - addPriorityPath
// is the mechanism that survives (it's what texture packs use).
static void ensureSearchPath() {
    static bool s_added = false;
    std::error_code ec;
    fs::create_directories(activeDir() / "icons", ec);
    if (!s_added) {
        s_added = true;
        CCFileUtils::get()->addPriorityPath(activeDir().string().c_str());
    }
}

// resolve a relative resource path while ignoring our own override directory
static std::string pathWithoutOverride(std::string const& rel) {
    auto fu = CCFileUtils::sharedFileUtils();
    auto ourKey = pathKey(activeDir().string());
    std::error_code ec;
    for (auto const& sp : fu->getSearchPaths()) {
        if (pathKey(std::string(sp)) == ourKey) continue;
        auto cand = fs::path(std::string(sp)) / rel;
        if (fs::exists(cand, ec)) return cand.string();
    }
    return "";
}

// ---------- cache surgery ----------

// cocos' addSpriteFramesWithDictionary SKIPS frames that already exist, so a
// sheet only truly reloads if every frame is first removed by name
static void removeFramesFromPlist(std::string const& plistFullPath) {
    auto dict = CCDictionary::createWithContentsOfFileThreadSafe(plistFullPath.c_str());
    if (!dict) return;
    if (auto frames = static_cast<CCDictionary*>(dict->objectForKey("frames"))) {
        auto sfc = CCSpriteFrameCache::sharedSpriteFrameCache();
        CCDictElement* el = nullptr;
        CCDICT_FOREACH(frames, el) {
            sfc->removeSpriteFrameByName(el->getStrKey());
        }
    }
    dict->release();
}

// evict the sheet's texture object no matter what cache key it sits under
static void dropSheetTexture(std::string const& base) {
    auto sfc = CCSpriteFrameCache::sharedSpriteFrameCache();
    auto tc = CCTextureCache::sharedTextureCache();
    for (auto const& probe : { fmt::format("{}_001.png", base), fmt::format("{}_01_001.png", base) }) {
        if (auto frame = sfc->spriteFrameByName(probe.c_str())) {
            if (auto tex = frame->getTexture()) tc->removeTexture(tex);
        }
    }
}

// drop every cached form of a sheet so the next load re-resolves the files
static void purgeSheet(std::string const& base) {
    auto fu = CCFileUtils::sharedFileUtils();
    auto sfc = CCSpriteFrameCache::sharedSpriteFrameCache();
    auto tc = CCTextureCache::sharedTextureCache();
    dropSheetTexture(base);
    for (auto dir : { "icons/", "" }) {
        for (auto suf : { "-uhd", "-hd", "" }) {
            auto plist = fmt::format("{}{}{}.plist", dir, base, suf);
            std::string full = fu->fullPathForFilename(plist.c_str(), true);
            if (full != plist && fu->isFileExist(full)) {
                removeFramesFromPlist(full);
                sfc->removeSpriteFramesFromFile(plist.c_str());
            } else if (auto game = pathWithoutOverride(plist); !game.empty()) {
                removeFramesFromPlist(game);
            }
            auto png = fmt::format("{}{}{}.png", dir, base, suf);
            std::string fullPng = fu->fullPathForFilename(png.c_str(), true);
            tc->removeTextureForKey(fullPng.c_str());
            tc->removeTextureForKey(png.c_str());
        }
    }
    fu->purgeCachedEntries();
}

// load the sheet back right away so the game's own icon bookkeeping (which may
// still consider it loaded) never sees missing frames
static void reloadSheet(std::string const& base) {
    auto fu = CCFileUtils::sharedFileUtils();
    for (auto dir : { "icons/", "" }) {
        auto plist = fmt::format("{}{}.plist", dir, base);
        auto png = fmt::format("{}{}.png", dir, base);
        std::string full = fu->fullPathForFilename(plist.c_str(), false);
        if (full != plist && fu->isFileExist(full)) {
            if (auto tex = CCTextureCache::sharedTextureCache()->addImage(png.c_str(), false)) {
                CCSpriteFrameCache::sharedSpriteFrameCache()->addSpriteFramesWithFile(plist.c_str(), tex);
            }
            return;
        }
    }
}

static void applyVariation(std::string const& base, std::string const& name) {
    ensureSearchPath();
    purgeSheet(base);

    std::error_code ec;
    auto act = activeDir();
    fs::path roots[] = { act, act / "icons" };
    const char* sufs[] = { "", "-hd", "-uhd" };

    for (auto const& root : roots) {
        fs::create_directories(root, ec);
        for (auto suf : sufs) {
            fs::remove(root / (base + suf + ".png"), ec);
            fs::remove(root / (base + suf + ".plist"), ec);
        }
    }

    if (!name.empty()) {
        auto src = variationsDir(base);
        auto png = src / (name + ".png");
        for (auto const& root : roots) {
            for (auto suf : sufs) {
                fs::copy_file(png, root / (base + suf + ".png"),
                    fs::copy_options::overwrite_existing, ec);
                auto plistSrc = src / (name + suf + ".plist");
                if (!fs::exists(plistSrc, ec)) plistSrc = src / (name + ".plist");
                if (fs::exists(plistSrc, ec)) {
                    fs::copy_file(plistSrc, root / (base + suf + ".plist"),
                        fs::copy_options::overwrite_existing, ec);
                }
            }
        }
    }

    Mod::get()->setSavedValue<std::string>("active_" + base, name);
    CCFileUtils::sharedFileUtils()->purgeCachedEntries();
    reloadSheet(base);
}

// point a user-supplied plist's texture reference at the sheet name the game
// will look up, so it keeps working across sessions
static std::string rewritePlist(std::string const& text, std::string const& base) {
    static std::regex re(
        R"((<key>\s*(?:realTextureFileName|textureFileName)\s*</key>\s*<string>)[^<]*(</string>))");
    return std::regex_replace(text, re, "$1" + base + ".png$2");
}

// ---------- UI ----------

class VariationsPopup : public Popup {
protected:
    IconType m_type = IconType::Cube;
    Ref<GJGarageLayer> m_garage;
    CCLabelBMFont* m_typeLabel = nullptr;
    CCLabelBMFont* m_iconLabel = nullptr;
    ScrollLayer* m_scroll = nullptr;
    std::vector<std::string> m_entries;
    fs::path m_pendingPng;

    bool init(GJGarageLayer* garage) {
        if (!Popup::init(320.f, 250.f)) return false;
        m_garage = garage;
        this->setTitle("Custom Icons");
        m_type = static_cast<IconType>(
            clampType(static_cast<int>(GameManager::get()->m_playerIconType)));

        auto menu = m_buttonMenu;

        auto leftSpr = CCSprite::createWithSpriteFrameName("GJ_arrow_01_001.png");
        leftSpr->setScale(0.65f);
        auto leftBtn = CCMenuItemSpriteExtra::create(
            leftSpr, this, menu_selector(VariationsPopup::onPrevType));
        leftBtn->setPosition({ 70.f, 200.f });
        menu->addChild(leftBtn);

        auto rightSpr = CCSprite::createWithSpriteFrameName("GJ_arrow_01_001.png");
        rightSpr->setFlipX(true);
        rightSpr->setScale(0.65f);
        auto rightBtn = CCMenuItemSpriteExtra::create(
            rightSpr, this, menu_selector(VariationsPopup::onNextType));
        rightBtn->setPosition({ 250.f, 200.f });
        menu->addChild(rightBtn);

        m_typeLabel = CCLabelBMFont::create("Cube", "bigFont.fnt");
        m_typeLabel->setScale(0.6f);
        m_typeLabel->setPosition({ 160.f, 200.f });
        m_mainLayer->addChild(m_typeLabel);

        m_iconLabel = CCLabelBMFont::create("", "goldFont.fnt");
        m_iconLabel->setScale(0.45f);
        m_iconLabel->setPosition({ 160.f, 180.f });
        m_mainLayer->addChild(m_iconLabel);

        auto bg = CCScale9Sprite::create("square02b_001.png");
        bg->setContentSize({ 250.f, 110.f });
        bg->setColor({ 0, 0, 0 });
        bg->setOpacity(90);
        bg->setPosition({ 160.f, 118.f });
        m_mainLayer->addChild(bg);

        m_scroll = ScrollLayer::create({ 250.f, 110.f });
        m_scroll->setPosition({ 35.f, 63.f });
        m_mainLayer->addChild(m_scroll);

        auto addSpr = ButtonSprite::create("Add Image");
        addSpr->setScale(0.7f);
        auto addBtn = CCMenuItemSpriteExtra::create(
            addSpr, this, menu_selector(VariationsPopup::onAdd));
        addBtn->setPosition({ 160.f, 35.f });
        menu->addChild(addBtn);

        this->refreshList();
        return true;
    }

    std::string currentBase() {
        return sheetBase(m_type, GameManager::get()->activeIconForType(m_type));
    }

    void refreshList() {
        auto base = this->currentBase();
        m_typeLabel->setString(TYPE_NAMES[clampType(static_cast<int>(m_type))]);
        m_iconLabel->setString(fmt::format("Sheet: {} (equipped icon)", base).c_str());

        m_entries.clear();
        m_entries.push_back(""); // Default
        std::error_code ec;
        auto dir = variationsDir(base);
        if (fs::exists(dir, ec)) {
            for (auto const& entry : fs::directory_iterator(dir, ec)) {
                if (entry.path().extension() == ".png") {
                    m_entries.push_back(entry.path().stem().string());
                }
            }
        }
        auto active = Mod::get()->getSavedValue<std::string>("active_" + base, "");

        auto content = m_scroll->m_contentLayer;
        content->removeAllChildren();
        float rowH = 34.f;
        float width = 250.f;
        float height = std::max(110.f, rowH * static_cast<float>(m_entries.size()));
        content->setContentSize({ width, height });

        for (size_t i = 0; i < m_entries.size(); ++i) {
            bool isActive = m_entries[i] == active;
            auto label = m_entries[i].empty() ? std::string("Default") : m_entries[i];
            if (isActive) label += "  <-";
            auto spr = ButtonSprite::create(label.c_str());
            spr->setScale(0.6f);
            if (!isActive) spr->setColor({ 140, 140, 140 });
            auto item = CCMenuItemSpriteExtra::create(
                spr, this, menu_selector(VariationsPopup::onSelect));
            item->setTag(static_cast<int>(i));
            auto rowMenu = CCMenu::create();
            rowMenu->setPosition({ width / 2, height - rowH * (static_cast<float>(i) + 0.5f) });
            rowMenu->addChild(item);
            content->addChild(rowMenu);
        }
        m_scroll->moveToTop();
        handleTouchPriority(this);
    }

    void refreshGarage() {
        if (m_garage && m_garage->m_playerObject) {
            m_garage->m_playerObject->updatePlayerFrame(
                GameManager::get()->activeIconForType(m_type), m_type);
        }
    }

    void onPrevType(CCObject*) {
        m_type = static_cast<IconType>((clampType(static_cast<int>(m_type)) + 8) % 9);
        this->refreshList();
    }
    void onNextType(CCObject*) {
        m_type = static_cast<IconType>((clampType(static_cast<int>(m_type)) + 1) % 9);
        this->refreshList();
    }

    void onSelect(CCObject* sender) {
        int idx = static_cast<CCNode*>(sender)->getTag();
        if (idx < 0 || idx >= static_cast<int>(m_entries.size())) return;
        applyVariation(this->currentBase(), m_entries[idx]);
        this->refreshList();
        this->refreshGarage();
    }

    void onAdd(CCObject*) {
        async::spawn(
            file::pick(file::PickMode::OpenFile, file::FilePickOptions {
                .filters = { file::FilePickOptions::Filter {
                    .description = "PNG image",
                    .files = { "*.png" },
                }}
            }),
            [self = Ref(this)](Result<std::optional<fs::path>> result) {
                if (result.isOk()) {
                    if (auto path = result.unwrap()) self->onImagePicked(*path);
                }
            }
        );
    }

    void onImagePicked(fs::path path) {
        m_pendingPng = path;
        createQuickPopup(
            "Plist Source",
            fmt::format(
                "Should <cy>{}</c> use this icon's <cg>existing</c> plist (your PNG must "
                "match the original sheet's layout), or your <co>own</c> plist file?",
                path.filename().string()),
            "Existing", "My Own",
            [this](auto, bool btn2) {
                if (btn2) this->pickPlist();
                else this->importVariation(std::nullopt);
            });
    }

    void pickPlist() {
        async::spawn(
            file::pick(file::PickMode::OpenFile, file::FilePickOptions {
                .filters = { file::FilePickOptions::Filter {
                    .description = "Plist file",
                    .files = { "*.plist" },
                }}
            }),
            [self = Ref(this)](Result<std::optional<fs::path>> result) {
                if (result.isOk()) {
                    if (auto path = result.unwrap()) self->importVariation(*path);
                }
            }
        );
    }

    void importVariation(std::optional<fs::path> plist) {
        auto base = this->currentBase();
        auto dir = variationsDir(base);
        std::error_code ec;
        fs::create_directories(dir, ec);

        auto name = m_pendingPng.stem().string();
        if (name.empty()) name = "custom";

        fs::copy_file(m_pendingPng, dir / (name + ".png"),
            fs::copy_options::overwrite_existing, ec);
        if (ec) {
            FLAlertLayer::create("Error", "Could not copy the image file.", "OK")->show();
            return;
        }

        // clear any plists left over from a previous import under the same name
        std::error_code ec2;
        for (auto suf : { "", "-hd", "-uhd" }) {
            fs::remove(dir / (name + suf + ".plist"), ec2);
        }

        if (plist) {
            auto text = file::readString(*plist);
            if (text.isErr()) {
                FLAlertLayer::create("Error", "Could not read the plist file.", "OK")->show();
                return;
            }
            auto res = file::writeString(dir / (name + ".plist"), rewritePlist(text.unwrap(), base));
            if (res.isErr()) {
                FLAlertLayer::create("Error", "Could not save the plist file.", "OK")->show();
                return;
            }
        } else {
            // snapshot the game's own plists (one per texture quality) so the
            // sheet keeps its original layout even when our override is active
            for (auto suf : { "", "-hd", "-uhd" }) {
                for (auto d : { "icons/", "" }) {
                    auto found = pathWithoutOverride(fmt::format("{}{}{}.plist", d, base, suf));
                    if (!found.empty()) {
                        fs::copy_file(found, dir / fmt::format("{}{}.plist", name, suf),
                            fs::copy_options::overwrite_existing, ec2);
                        break;
                    }
                }
            }
        }

        applyVariation(base, name);
        this->refreshList();
        this->refreshGarage();
        Notification::create(fmt::format("Added skin '{}'", name), NotificationIcon::Success)->show();
    }

public:
    static VariationsPopup* create(GJGarageLayer* garage) {
        auto ret = new VariationsPopup();
        if (ret->init(garage)) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

// ---------- hooks ----------

struct $modify(CIGarageLayer, GJGarageLayer) {
    bool init() {
        if (!GJGarageLayer::init()) return false;
        auto spr = ButtonSprite::create("Skins");
        spr->setScale(0.55f);
        auto btn = CCMenuItemSpriteExtra::create(
            spr, this, menu_selector(CIGarageLayer::onCustomIcons));
        btn->setID("skins-button"_spr);
        auto menu = CCMenu::create();
        auto win = CCDirector::sharedDirector()->getWinSize();
        menu->setPosition({ 42.f, win.height - 70.f });
        menu->addChild(btn);
        this->addChild(menu, 10);
        return true;
    }
    void onCustomIcons(CCObject*) {
        VariationsPopup::create(this)->show();
    }
};

struct $modify(CILoadingLayer, LoadingLayer) {
    bool init(bool fromReload) {
        ensureSearchPath();
        if (!LoadingLayer::init(fromReload)) return false;
        ensureSearchPath();
        return true;
    }
};

$on_mod(Loaded) {
    ensureSearchPath();
}
