#include "LanguageIds.h"
namespace popopt::lang {
const wchar_t* Name(int id)
{
    static const wchar_t* names[] = {
        L"None/Auto", L"English", L"French", L"Spanish", L"Polish", L"German",
        L"Chinese", L"Hungarian", L"Italian", L"Japanese", L"Czech",
        L"Korean", L"Russian", L"Dutch"
    };
    return (id >= 0 && id <= 13) ? names[id] : L"Unknown";
}
const wchar_t* VoiceSuffix(int id)
{
    static const wchar_t* suffixes[] = {
        L"", L"Eng", L"Fre", L"Spa", L"Pol", L"Ger", L"Chi",
        L"Hun", L"Ita", L"Jap", L"Cze", L"Kor", L"Rus", L"Dut"
    };
    return (id >= 1 && id <= 13) ? suffixes[id] : L"";
}
}
