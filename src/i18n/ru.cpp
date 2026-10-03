// Russian translation.
#include "i18n/i18n.h"

namespace firefox_module {

namespace {
const facet::i18n::Table& ru() {
    static const facet::i18n::Table table = {
        {"Not a web address: {}", "Это не веб-адрес: {}"},
        {"Could not start Firefox.", "Не удалось запустить Firefox."},
        {"Browser for apps", "Браузер для приложений"},
        {"Web pages for apps", "Веб-страницы для приложений"},
        {"Firefox is made by Mozilla and downloaded unchanged from mozilla.org; this module is not a Mozilla "
         "product. Using Firefox means accepting Mozilla's terms: mozilla.org/about/legal/terms/firefox",
         "Firefox делает Mozilla, он скачивается с mozilla.org без изменений; этот модуль не продукт Mozilla. "
         "Пользуясь Firefox, вы принимаете условия Mozilla: mozilla.org/about/legal/terms/firefox"},
        {"Apps that show web pages use this Firefox. Each app has its own profile: logins and cookies are not "
         "shared between apps.",
         "Приложения, которые показывают веб-страницы, используют этот Firefox. У каждого приложения свой профиль: "
         "входы и куки не смешиваются."},
        {"Downloading", "Загрузка"},
        {"Checking the download", "Проверка загрузки"},
        {"Unpacking", "Распаковка"},
        {"Cancel", "Отмена"},
        {"Firefox", "Firefox"},
        {"not installed", "не установлен"},
        {"It is downloaded from Mozilla (about 80 MB, 270 MB on disk) when an app first needs it, or now:",
         "Он скачивается с сайта Mozilla (около 80 МБ, на диске 270 МБ), когда он впервые понадобится приложению, "
         "или сейчас:"},
        {"Install Firefox", "Установить Firefox"},
        {"Available", "Доступна"},
        {"Update (open pages restart)", "Обновить (открытые страницы перезапустятся)"},
        {"Error", "Ошибка"},
        {"Open pages", "Открытые страницы"},
        {"None right now.", "Сейчас нет."},
        {"Storage", "Хранилище"},
        {"Remove Firefox (apps download it again when needed)",
         "Удалить Firefox (приложения скачают его снова, когда он понадобится)"},
    };
    return table;
}
}  // namespace

void register_translations(facet::i18n::Catalog& catalog) { catalog.add("ru", ru()); }

}  // namespace firefox_module
