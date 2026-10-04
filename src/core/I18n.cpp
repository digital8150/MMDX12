#include "core/I18n.h"

#include <atomic>
#include <string>
#include <string_view>
#include <unordered_map>

#ifdef _WIN32
#include <windows.h>
#endif

namespace mmdx {

namespace {

std::atomic<int> g_lang{(int)Language::Korean};

using Table = std::unordered_map<std::string_view, const char*>;

const Table& TableFor(Language l) {
    static const Table en = [] {
        Table t;
        for (int i = 0; i < i18n::kEnglishCount; ++i) t.emplace(i18n::kEnglish[i].ko, i18n::kEnglish[i].tr);
        return t;
    }();
    static const Table zh = [] {
        Table t;
        for (int i = 0; i < i18n::kChineseCount; ++i) t.emplace(i18n::kChinese[i].ko, i18n::kChinese[i].tr);
        return t;
    }();
    static const Table ja = [] {
        Table t;
        for (int i = 0; i < i18n::kJapaneseCount; ++i) t.emplace(i18n::kJapanese[i].ko, i18n::kJapanese[i].tr);
        return t;
    }();
    return l == Language::Japanese ? ja : l == Language::Chinese ? zh : en;
}

} // namespace

Language DetectSystemLanguage() {
#ifdef _WIN32
    wchar_t name[LOCALE_NAME_MAX_LENGTH] = {};
    if (GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH) > 0) {
        const std::wstring_view n(name);
        if (n.substr(0, 2) == L"ko") return Language::Korean;
        if (n.substr(0, 2) == L"ja") return Language::Japanese;
        if (n.substr(0, 2) == L"zh") return Language::Chinese;
    }
#endif
    return Language::English;
}

void SetLanguage(Language lang) {
    if (lang == Language::Auto) lang = DetectSystemLanguage();
    g_lang.store((int)lang);
}

Language ActiveLanguage() { return (Language)g_lang.load(); }

const char* LanguageName(Language lang) {
    switch (lang) {
    case Language::Korean: return "한국어";
    case Language::English: return "English";
    case Language::Japanese: return "日本語";
    case Language::Chinese: return "简体中文";
    default: return "Auto";
    }
}

const char* Tr(const char* ko) {
    const Language l = ActiveLanguage();
    if (l == Language::Korean || !ko) return ko;
    const Table& t = TableFor(l);
    const auto it = t.find(std::string_view(ko));
    return it != t.end() ? it->second : ko;
}

} // namespace mmdx
