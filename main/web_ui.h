#ifndef WEB_UI_H
#define WEB_UI_H

#include <stddef.h>

typedef struct {
    const char *content;
    size_t length;
    const char *content_type;
} web_ui_asset_t;

const web_ui_asset_t *web_ui_html_asset(void);
const web_ui_asset_t *web_ui_css_asset(void);
const web_ui_asset_t *web_ui_js_asset(void);

#endif
