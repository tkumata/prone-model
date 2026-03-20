#include "web_ui.h"

extern const char HTML_START[] asm("_binary_index_html_start");
extern const char HTML_END[] asm("_binary_index_html_end");
extern const char CSS_START[] asm("_binary_app_css_start");
extern const char CSS_END[] asm("_binary_app_css_end");
extern const char JS_START[] asm("_binary_app_js_start");
extern const char JS_END[] asm("_binary_app_js_end");

static size_t embedded_asset_length(const char *start, const char *end)
{
    size_t length = (size_t) (end - start);

    return length > 0 ? length - 1 : 0;
}

const web_ui_asset_t *web_ui_html_asset(void)
{
    static web_ui_asset_t asset;

    asset.content = HTML_START;
    asset.length = embedded_asset_length(HTML_START, HTML_END);
    asset.content_type = "text/html; charset=utf-8";
    return &asset;
}

const web_ui_asset_t *web_ui_css_asset(void)
{
    static web_ui_asset_t asset;

    asset.content = CSS_START;
    asset.length = embedded_asset_length(CSS_START, CSS_END);
    asset.content_type = "text/css; charset=utf-8";
    return &asset;
}

const web_ui_asset_t *web_ui_js_asset(void)
{
    static web_ui_asset_t asset;

    asset.content = JS_START;
    asset.length = embedded_asset_length(JS_START, JS_END);
    asset.content_type = "application/javascript; charset=utf-8";
    return &asset;
}
