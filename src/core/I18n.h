#pragma once
#include <string>

namespace mmdx {

// UI language. Korean is the source language: Tr() takes the Korean text as its key and returns
// the translation (or the key itself for Korean / a missing entry). Tables: I18nEn.cpp, I18nJa.cpp.
enum class Language { Auto = 0, Korean = 1, English = 2, Japanese = 3, Chinese = 4 };

// Language of the Windows UI (Korean / Japanese / Chinese, otherwise English).
Language DetectSystemLanguage();

// Sets the active language (Auto resolves through DetectSystemLanguage). Thread-safe.
void SetLanguage(Language lang);
Language ActiveLanguage();  // never Auto

// Native name for the language picker ("한국어", "English", "日本語").
const char* LanguageName(Language lang);

const char* Tr(const char* ko);
inline const char* Tr(const std::string& ko) { return Tr(ko.c_str()); }

namespace i18n {
struct Entry { const char* ko; const char* tr; };
extern const Entry kEnglish[];
extern const int kEnglishCount;
extern const Entry kJapanese[];
extern const int kJapaneseCount;
extern const Entry kChinese[];
extern const int kChineseCount;
}

} // namespace mmdx
