// Russian translation of the example.
#include "i18n/i18n.h"

namespace web_app {

namespace {
const facet::i18n::Table& ru() {
    static const facet::i18n::Table table = {
        {"Preparing the browser…", "Готовлю браузер…"},
        {"Web app", "Веб-приложение"},
        {"Address", "Адрес"},
        {"Installing Firefox", "Установка Firefox"},
        {"State", "Состояние"},
        {"connecting…", "подключение…"},
        {"Error", "Ошибка"},
    };
    return table;
}
}  // namespace

void register_translations(facet::i18n::Catalog& catalog) { catalog.add("ru", ru()); }

}  // namespace web_app
