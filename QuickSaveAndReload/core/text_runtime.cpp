#include "pch.h"

#include "include/text_runtime.h"

namespace qsr::text {
namespace {

struct LocalizedEntry {
    TextId id;
    const wchar_t* en;
    const wchar_t* ko;
    const wchar_t* fr;
    const wchar_t* pt_br;
    const wchar_t* ru;
};

std::array<std::string, static_cast<std::size_t>(TextId::Count)> g_text{};

std::string NarrowWide(const wchar_t* text) {
    if (text == nullptr || text[0] == L'\0') {
        return {};
    }

    const int required = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (required <= 1) {
        return {};
    }

    std::string result(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), required, nullptr, nullptr);
    result.resize(static_cast<std::size_t>(required - 1));
    return result;
}

const wchar_t* SelectText(const LocalizedEntry& entry, const std::wstring& locale) {
    if (_wcsicmp(locale.c_str(), L"ko_KR") == 0
        || _wcsicmp(locale.c_str(), L"ko") == 0
        || _wcsicmp(locale.c_str(), L"kr") == 0
        || _wcsicmp(locale.c_str(), L"korean") == 0) {
        return entry.ko;
    }
    if (_wcsicmp(locale.c_str(), L"fr_FR") == 0
        || _wcsicmp(locale.c_str(), L"fr") == 0
        || _wcsicmp(locale.c_str(), L"french") == 0
        || _wcsicmp(locale.c_str(), L"francais") == 0
        || _wcsicmp(locale.c_str(), L"français") == 0) {
        return entry.fr;
    }
    if (_wcsicmp(locale.c_str(), L"pt_BR") == 0
        || _wcsicmp(locale.c_str(), L"pt-BR") == 0
        || _wcsicmp(locale.c_str(), L"ptbr") == 0
        || _wcsicmp(locale.c_str(), L"pt") == 0
        || _wcsicmp(locale.c_str(), L"portuguese") == 0
        || _wcsicmp(locale.c_str(), L"brazilian_portuguese") == 0
        || _wcsicmp(locale.c_str(), L"portuguese_brazil") == 0) {
        return entry.pt_br;
    }
    if (_wcsicmp(locale.c_str(), L"ru_RU") == 0
        || _wcsicmp(locale.c_str(), L"ru-RU") == 0
        || _wcsicmp(locale.c_str(), L"ru") == 0
        || _wcsicmp(locale.c_str(), L"russian") == 0) {
        return entry.ru;
    }
    return entry.en;
}

}  // namespace

void Initialize(const std::wstring& locale) {
    static const LocalizedEntry kEntries[] = {
        {TextId::UiRowLabel, L"Quick Save", L"빠른 저장", L"Sauvegarde rapide", L"Salvamento Rápido", L"Быстрое сохранение"},
        {TextId::ToastQuickSaveSuccess, L"QUICK SAVE SUCCESS", L"빠른 저장을 완료했습니다.", L"SUCCÈS DE LA SAUVEGARDE RAPIDE", L"SALVAMENTO RÁPIDO CONCLUÍDO", L"Успешно сохранено"},
        {TextId::ToastQuickSaveFailed, L"QUICK SAVE FAILED", L"빠른 저장을 실패했습니다.", L"ÉCHEC DE LA SAUVEGARDE RAPIDE", L"SALVAMENTO RÁPIDO FALHOU", L"Не удалось сохранить"},
        {TextId::ToastSaveFunctionUnavailable, L"SAVE FUNCTION UNAVAILABLE", L"저장 기능을 사용할 수 없습니다.", L"SAUVEGARDE INDISPONIBLE", L"FUNÇÃO SALVAR INDISPONÍVEL", L"Сохранение недоступно"},
        {TextId::ToastNoSaveActor, L"NO SAVE ACTOR", L"유효한 저장 객체를 찾을 수 없습니다.", L"PAS D'ENTITÉ DE SAUVEGARDE", L"SEM ATOR DE SALVAMENTO", L"Нет объекта для сохранения"},
        {TextId::ToastQuickSaveFailedCode, L"QUICK SAVE FAILED (%u)", L"빠른 저장을 실패했습니다. (%u)", L"ÉCHEC DE LA SAUVEGARDE RAPIDE (%u)", L"SALVAMENTO RÁPIDO FALHOU (%u)", L"Не удалось сохранить (%u)"},
        {TextId::ToastQuickLoadFailed, L"QUICK LOAD FAILED", L"빠른 불러오기에 실패했습니다.", L"ÉCHEC DU CHARGEMENT RAPIDE", L"FALHA NO CARREGAMENTO RÁPIDO", L"Не удалось загрузить"},
        {TextId::ToastLoadFunctionUnavailable, L"LOAD FUNCTION UNAVAILABLE", L"불러오기 기능을 사용할 수 없습니다.", L"CHARGEMENT INDISPONIBLE", L"FUNÇÃO DE CARREGAMENTO INDISPONÍVEL", L"Загрузка недоступна"},
        {TextId::ToastNoQuickSaveFound, L"NO QUICK SAVE FOUND", L"빠른 저장 데이터를 찾을 수 없습니다.", L"SAUVEGARDE RAPIDE INTROUVABLE", L"NENHUM SALVAMENTO RÁPIDO ENCONTRADO", L"Быстрое сохранение не найдено"},
        {TextId::ToastGameStateUnavailable, L"GAME STATE UNAVAILABLE", L"게임 상태를 확인할 수 없습니다.", L"CONTEXTE DU JEU INDISPONIBLE", L"ESTADO DE JOGO INDISPONÍVEL", L"Состояние игры недоступно"},
    };

    for (const LocalizedEntry& entry : kEntries) {
        g_text[static_cast<std::size_t>(entry.id)] = NarrowWide(SelectText(entry, locale));
    }
}

const char* Get(TextId id) {
    const std::size_t index = static_cast<std::size_t>(id);
    if (index >= g_text.size() || g_text[index].empty()) {
        return "";
    }
    return g_text[index].c_str();
}

std::string Format(TextId id, unsigned int value) {
    char buffer[128] = {};
    std::snprintf(buffer, sizeof(buffer), Get(id), value);
    buffer[sizeof(buffer) - 1] = '\0';
    return buffer;
}

}  // namespace qsr::text
