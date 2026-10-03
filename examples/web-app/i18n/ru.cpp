// Russian translation of the example.
#include "i18n/i18n.h"

namespace web_app {

namespace {
const facet::i18n::Table& ru() {
    static const facet::i18n::Table table = {
        {"Getting ready…", "Подготовка…"},
        {"Web app", "Веб-приложение"},
        {"Address", "Адрес"},
        {"The web view module is getting ready", "Модуль веб-страниц готовится"},
        {"State", "Состояние"},
        {"waiting for the web view module…", "ожидание модуля веб-страниц…"},
        {"opening again…", "открываю снова…"},
        {"opening…", "открываю…"},
        {"Error", "Ошибка"},
    };
    return table;
}
}  // namespace

void register_translations(facet::i18n::Catalog& catalog) { catalog.add("ru", ru()); }

}  // namespace web_app
