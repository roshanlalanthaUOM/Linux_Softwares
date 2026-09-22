/*
 * simplebrowser - a minimal web browser built with GTK3 + WebKitGTK
 *
 * v1: single window, single tab.
 *   - Address bar (GtkEntry) -> press Enter to load
 *   - Back / Forward / Reload / Stop toolbar buttons
 *   - Window title tracks the page title
 *   - Address bar updates as navigation happens
 *
 * Build: see Makefile (uses pkg-config for gtk+-3.0 and webkit2gtk-4.1)
 */

#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <string.h>

/* Bundle the widgets we need to reach from callbacks */
typedef struct {
    GtkWidget *window;
    GtkWidget *url_entry;
    GtkWidget *back_btn;
    GtkWidget *forward_btn;
    GtkWidget *reload_btn;
    WebKitWebView *web_view;
} BrowserWidgets;

/* Make sure the URL has a scheme; if not, decide between a bare domain
 * (add https://) and a search query (send to a search engine). */
static gchar *normalize_url(const char *input) {
    if (!input || strlen(input) == 0) {
        return g_strdup("about:blank");
    }

    if (g_str_has_prefix(input, "http://") ||
        g_str_has_prefix(input, "https://") ||
        g_str_has_prefix(input, "file://") ||
        g_str_has_prefix(input, "about:")) {
        return g_strdup(input);
    }

    /* Looks like "example.com" or "example.com/path" -> assume https */
    gboolean looks_like_domain =
        (strchr(input, ' ') == NULL) &&
        (strchr(input, '.') != NULL);

    if (looks_like_domain) {
        return g_strdup_printf("https://%s", input);
    }

    /* Otherwise treat it as a search query */
    gchar *escaped = g_uri_escape_string(input, NULL, FALSE);
    gchar *url = g_strdup_printf("https://duckduckgo.com/?q=%s", escaped);
    g_free(escaped);
    return url;
}

/* --- Signal handlers -------------------------------------------------- */

static void on_url_activate(GtkEntry *entry, gpointer user_data) {
    BrowserWidgets *b = (BrowserWidgets *)user_data;
    const char *text = gtk_entry_get_text(entry);
    gchar *url = normalize_url(text);
    webkit_web_view_load_uri(b->web_view, url);
    g_free(url);
}

static void on_back_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    BrowserWidgets *b = (BrowserWidgets *)user_data;
    webkit_web_view_go_back(b->web_view);
}

static void on_forward_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    BrowserWidgets *b = (BrowserWidgets *)user_data;
    webkit_web_view_go_forward(b->web_view);
}

static void on_reload_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    BrowserWidgets *b = (BrowserWidgets *)user_data;
    webkit_web_view_reload(b->web_view);
}

static void on_stop_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    BrowserWidgets *b = (BrowserWidgets *)user_data;
    webkit_web_view_stop_loading(b->web_view);
}

/* Keep the address bar and window title in sync with the page */
static void on_uri_changed(WebKitWebView *web_view, GParamSpec *pspec,
                            gpointer user_data) {
    (void)pspec;
    BrowserWidgets *b = (BrowserWidgets *)user_data;
    const char *uri = webkit_web_view_get_uri(web_view);
    if (uri) {
        gtk_entry_set_text(GTK_ENTRY(b->url_entry), uri);
    }
    gtk_widget_set_sensitive(b->back_btn,
        webkit_web_view_can_go_back(web_view));
    gtk_widget_set_sensitive(b->forward_btn,
        webkit_web_view_can_go_forward(web_view));
}

static void on_title_changed(WebKitWebView *web_view, GParamSpec *pspec,
                              gpointer user_data) {
    (void)pspec;
    BrowserWidgets *b = (BrowserWidgets *)user_data;
    const char *title = webkit_web_view_get_title(web_view);
    gchar *window_title = g_strdup_printf("%s - simplebrowser",
        (title && strlen(title) > 0) ? title : "New Tab");
    gtk_window_set_title(GTK_WINDOW(b->window), window_title);
    g_free(window_title);
}

/* --- UI construction ---------------------------------------------------*/

static GtkWidget *make_toolbar(BrowserWidgets *b) {
    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(toolbar), 4);

    b->back_btn = gtk_button_new_from_icon_name("go-previous-symbolic",
        GTK_ICON_SIZE_BUTTON);
    b->forward_btn = gtk_button_new_from_icon_name("go-next-symbolic",
        GTK_ICON_SIZE_BUTTON);
    b->reload_btn = gtk_button_new_from_icon_name("view-refresh-symbolic",
        GTK_ICON_SIZE_BUTTON);
    GtkWidget *stop_btn = gtk_button_new_from_icon_name("process-stop-symbolic",
        GTK_ICON_SIZE_BUTTON);

    b->url_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(b->url_entry),
        "Enter a URL or search term");

    gtk_box_pack_start(GTK_BOX(toolbar), b->back_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), b->forward_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), b->reload_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), stop_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), b->url_entry, TRUE, TRUE, 0);

    g_signal_connect(b->back_btn, "clicked", G_CALLBACK(on_back_clicked), b);
    g_signal_connect(b->forward_btn, "clicked", G_CALLBACK(on_forward_clicked), b);
    g_signal_connect(b->reload_btn, "clicked", G_CALLBACK(on_reload_clicked), b);
    g_signal_connect(stop_btn, "clicked", G_CALLBACK(on_stop_clicked), b);
    g_signal_connect(b->url_entry, "activate", G_CALLBACK(on_url_activate), b);

    return toolbar;
}

static void activate(GtkApplication *app, gpointer user_data) {
    (void)user_data;
    BrowserWidgets *b = g_new0(BrowserWidgets, 1);

    b->window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(b->window), "simplebrowser");
    gtk_window_set_default_size(GTK_WINDOW(b->window), 1100, 750);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(b->window), vbox);

    GtkWidget *toolbar = make_toolbar(b);
    gtk_box_pack_start(GTK_BOX(vbox), toolbar, FALSE, FALSE, 0);

    b->web_view = WEBKIT_WEB_VIEW(webkit_web_view_new());
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(b->web_view), TRUE, TRUE, 0);

    g_signal_connect(b->web_view, "notify::uri",
        G_CALLBACK(on_uri_changed), b);
    g_signal_connect(b->web_view, "notify::title",
        G_CALLBACK(on_title_changed), b);

    /* Keyboard shortcut: Ctrl+L focuses the address bar */
    GtkWidget *entry = b->url_entry;
    GtkAccelGroup *accel_group = gtk_accel_group_new();
    gtk_window_add_accel_group(GTK_WINDOW(b->window), accel_group);
    gtk_widget_add_accelerator(entry, "grab-focus", accel_group,
        GDK_KEY_l, GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE);

    gtk_widget_show_all(b->window);

    webkit_web_view_load_uri(b->web_view, "https://duckduckgo.com");
    gtk_widget_grab_focus(GTK_WIDGET(b->web_view));
}

int main(int argc, char *argv[]) {
    GtkApplication *app = gtk_application_new(
        "com.electroeveryday.simplebrowser", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
