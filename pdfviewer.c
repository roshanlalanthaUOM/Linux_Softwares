/*
 * pdfviewer - a basic PDF viewer for Linux.
 *
 * Dependencies: GTK3 and poppler-glib (the GLib bindings for the Poppler
 * PDF rendering library). Both are common on Linux desktops.
 *
 * Build:  gcc -O2 -o pdfviewer pdfviewer.c $(pkg-config --cflags --libs gtk+-3.0 poppler-glib)
 *
 * Usage:  pdfviewer [file.pdf]
 *
 * Features: page navigation, zoom in/out, fit width/page, Ctrl+scroll to
 * zoom, keyboard shortcuts, encrypted-PDF password prompt.
 */

#include <gtk/gtk.h>
#include <poppler.h>
#include <math.h>
#include <string.h>

#define PAGE_MARGIN   18.0   /* grey border (in device pixels) around the page */
#define MIN_ZOOM      0.15
#define MAX_ZOOM      6.0
#define ZOOM_STEP     1.15

typedef struct {
    GtkWidget        *window;
    GtkWidget        *scroller;
    GtkWidget        *drawing_area;
    GtkWidget        *page_entry;
    GtkWidget        *page_count_label;
    GtkWidget        *zoom_label;
    GtkWidget        *statusbar;
    guint             status_ctx;

    PopplerDocument  *doc;
    gchar            *doc_path;     /* for the window title */
    int               n_pages;
    int               current_page; /* 0-based internally, shown 1-based */
    double            zoom;
    double            page_w, page_h; /* unscaled page size, in points */
} App;

static void load_page_into_view(App *app);
static gboolean open_path(App *app, const char *path);
static void build_ui(App *app, GtkApplication *gtk_app);

/* ------------------------------------------------------------------ */
/* Small helpers                                                      */
/* ------------------------------------------------------------------ */

static void show_error(GtkWindow *parent, const char *msg)
{
    GtkWidget *d = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL,
                                          GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
                                          "%s", msg);
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
}

static void set_status(App *app, const char *fmt, ...)
{
    va_list ap;
    char buf[512];
    va_start(ap, fmt);
    g_vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    gtk_statusbar_pop(GTK_STATUSBAR(app->statusbar), app->status_ctx);
    gtk_statusbar_push(GTK_STATUSBAR(app->statusbar), app->status_ctx, buf);
}

/* Asks for a password for an encrypted PDF. Returns a newly-allocated
 * string (caller frees), or NULL if the user cancelled. */
static gchar *prompt_password(GtkWindow *parent, const char *filename)
{
    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        "Password required", parent, GTK_DIALOG_MODAL,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Unlock", GTK_RESPONSE_OK, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);

    char label_text[400];
    g_snprintf(label_text, sizeof(label_text),
              "\"%s\" is password protected.", filename);
    GtkWidget *label = gtk_label_new(label_text);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);

    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(entry), FALSE);
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);

    gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(content), box);
    gtk_widget_show_all(dialog);

    gchar *result = NULL;
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK)
        result = g_strdup(gtk_entry_get_text(GTK_ENTRY(entry)));

    gtk_widget_destroy(dialog);
    return result;
}

/* ------------------------------------------------------------------ */
/* Rendering                                                           */
/* ------------------------------------------------------------------ */

static gboolean on_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
    App *app = (App *)data;
    int w = gtk_widget_get_allocated_width(widget);
    int h = gtk_widget_get_allocated_height(widget);

    /* background */
    cairo_set_source_rgb(cr, 0.55, 0.55, 0.58);
    cairo_paint(cr);

    if (!app->doc)
        return FALSE;

    double pw = app->page_w * app->zoom;
    double ph = app->page_h * app->zoom;

    /* drop shadow */
    cairo_set_source_rgba(cr, 0, 0, 0, 0.35);
    cairo_rectangle(cr, PAGE_MARGIN + 4, PAGE_MARGIN + 4, pw, ph);
    cairo_fill(cr);

    /* white page */
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_rectangle(cr, PAGE_MARGIN, PAGE_MARGIN, pw, ph);
    cairo_fill(cr);

    PopplerPage *page = poppler_document_get_page(app->doc, app->current_page);
    if (page) {
        cairo_save(cr);
        cairo_translate(cr, PAGE_MARGIN, PAGE_MARGIN);
        cairo_scale(cr, app->zoom, app->zoom);
        cairo_rectangle(cr, 0, 0, app->page_w, app->page_h);
        cairo_clip(cr);
        poppler_page_render(page, cr);
        cairo_restore(cr);
        g_object_unref(page);
    }

    (void)w; (void)h;
    return FALSE;
}

/* Recomputes the drawing area's size for the current page/zoom, updates
 * the toolbar labels, and asks GTK to redraw. */
static void refresh_view(App *app)
{
    if (!app->doc) {
        gtk_widget_set_size_request(app->drawing_area, -1, -1);
        gtk_entry_set_text(GTK_ENTRY(app->page_entry), "");
        gtk_label_set_text(GTK_LABEL(app->page_count_label), "/ 0");
        gtk_label_set_text(GTK_LABEL(app->zoom_label), "--");
        gtk_widget_queue_draw(app->drawing_area);
        return;
    }

    int w = (int)ceil(app->page_w * app->zoom + PAGE_MARGIN * 2);
    int h = (int)ceil(app->page_h * app->zoom + PAGE_MARGIN * 2);
    gtk_widget_set_size_request(app->drawing_area, w, h);

    char buf[32];
    g_snprintf(buf, sizeof(buf), "%d", app->current_page + 1);
    gtk_entry_set_text(GTK_ENTRY(app->page_entry), buf);

    g_snprintf(buf, sizeof(buf), "/ %d", app->n_pages);
    gtk_label_set_text(GTK_LABEL(app->page_count_label), buf);

    g_snprintf(buf, sizeof(buf), "%d%%", (int)lround(app->zoom * 100));
    gtk_label_set_text(GTK_LABEL(app->zoom_label), buf);

    gtk_widget_queue_draw(app->drawing_area);
}

static void goto_page(App *app, int page0)
{
    if (!app->doc)
        return;
    if (page0 < 0)
        page0 = 0;
    if (page0 >= app->n_pages)
        page0 = app->n_pages - 1;
    app->current_page = page0;
    load_page_into_view(app);
}

static void load_page_into_view(App *app)
{
    PopplerPage *page = poppler_document_get_page(app->doc, app->current_page);
    if (page) {
        poppler_page_get_size(page, &app->page_w, &app->page_h);
        g_object_unref(page);
    }
    refresh_view(app);
    /* scroll back to the top of the new page */
    GtkAdjustment *vadj =
        gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(app->scroller));
    gtk_adjustment_set_value(vadj, gtk_adjustment_get_lower(vadj));
}

static void set_zoom(App *app, double z)
{
    if (z < MIN_ZOOM) z = MIN_ZOOM;
    if (z > MAX_ZOOM) z = MAX_ZOOM;
    app->zoom = z;
    refresh_view(app);
}

/* ------------------------------------------------------------------ */
/* Opening documents                                                   */
/* ------------------------------------------------------------------ */

static void close_document(App *app)
{
    if (app->doc) {
        g_object_unref(app->doc);
        app->doc = NULL;
    }
    g_free(app->doc_path);
    app->doc_path = NULL;
    app->n_pages = 0;
    app->current_page = 0;
    gtk_window_set_title(GTK_WINDOW(app->window), "PDF Viewer");
    refresh_view(app);
    set_status(app, "No document open");
}

static gboolean open_path(App *app, const char *path)
{
    GFile *file = g_file_new_for_commandline_arg(path);
    gchar *uri = g_file_get_uri(file);
    gchar *display_name = g_file_get_basename(file);
    g_object_unref(file);

    GError *error = NULL;
    gchar *password = NULL;
    PopplerDocument *doc = NULL;

    for (;;) {
        doc = poppler_document_new_from_file(uri, password, &error);
        g_free(password);
        password = NULL;
        if (doc)
            break;
        if (error && error->domain == POPPLER_ERROR &&
            error->code == POPPLER_ERROR_ENCRYPTED) {
            g_clear_error(&error);
            password = prompt_password(GTK_WINDOW(app->window), display_name);
            if (!password) {          /* user hit Cancel */
                g_free(uri);
                g_free(display_name);
                return FALSE;
            }
            continue;
        }
        break;
    }

    if (!doc) {
        char msg[600];
        g_snprintf(msg, sizeof(msg), "Could not open \"%s\":\n%s",
                  display_name, error ? error->message : "unknown error");
        show_error(GTK_WINDOW(app->window), msg);
        g_clear_error(&error);
        g_free(uri);
        g_free(display_name);
        return FALSE;
    }

    close_document(app);
    app->doc = doc;
    app->doc_path = uri; /* keep the uri around, drop display_name */
    app->n_pages = poppler_document_get_n_pages(doc);
    app->current_page = 0;
    app->zoom = 1.0;

    char title[600];
    g_snprintf(title, sizeof(title), "%s - PDF Viewer", display_name);
    gtk_window_set_title(GTK_WINDOW(app->window), title);
    set_status(app, "Opened %s (%d page%s)", display_name, app->n_pages,
              app->n_pages == 1 ? "" : "s");
    g_free(display_name);

    load_page_into_view(app);
    return TRUE;
}

static void open_dialog(App *app)
{
    GtkWidget *dialog = gtk_file_chooser_dialog_new(
        "Open PDF", GTK_WINDOW(app->window), GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, NULL);

    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "PDF documents");
    gtk_file_filter_add_mime_type(filter, "application/pdf");
    gtk_file_filter_add_pattern(filter, "*.pdf");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        gchar *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        gtk_widget_destroy(dialog);
        if (path) {
            open_path(app, path);
            g_free(path);
        }
    } else {
        gtk_widget_destroy(dialog);
    }
}

/* ------------------------------------------------------------------ */
/* Fit helpers                                                         */
/* ------------------------------------------------------------------ */

static void fit_width(App *app)
{
    if (!app->doc)
        return;
    int avail = gtk_widget_get_allocated_width(app->scroller) -
               (int)(PAGE_MARGIN * 2) - 4;
    if (avail > 0)
        set_zoom(app, avail / app->page_w);
}

static void fit_page(App *app)
{
    if (!app->doc)
        return;
    int aw = gtk_widget_get_allocated_width(app->scroller) -
            (int)(PAGE_MARGIN * 2) - 4;
    int ah = gtk_widget_get_allocated_height(app->scroller) -
            (int)(PAGE_MARGIN * 2) - 4;
    if (aw > 0 && ah > 0)
        set_zoom(app, fmin(aw / app->page_w, ah / app->page_h));
}

/* ------------------------------------------------------------------ */
/* Callbacks                                                           */
/* ------------------------------------------------------------------ */

static void on_open_clicked(GtkButton *b, gpointer data) { (void)b; open_dialog((App *)data); }
static void on_prev_clicked(GtkButton *b, gpointer data) { (void)b; App *a = data; goto_page(a, a->current_page - 1); }
static void on_next_clicked(GtkButton *b, gpointer data) { (void)b; App *a = data; goto_page(a, a->current_page + 1); }
static void on_zoom_in(GtkButton *b, gpointer data)  { (void)b; App *a = data; set_zoom(a, a->zoom * ZOOM_STEP); }
static void on_zoom_out(GtkButton *b, gpointer data) { (void)b; App *a = data; set_zoom(a, a->zoom / ZOOM_STEP); }
static void on_zoom_reset(GtkButton *b, gpointer data){ (void)b; set_zoom((App *)data, 1.0); }
static void on_fit_width(GtkButton *b, gpointer data) { (void)b; fit_width((App *)data); }
static void on_fit_page(GtkButton *b, gpointer data)  { (void)b; fit_page((App *)data); }

static void on_page_entry_activate(GtkEntry *entry, gpointer data)
{
    App *app = (App *)data;
    const char *text = gtk_entry_get_text(entry);
    int page = atoi(text);
    if (page >= 1)
        goto_page(app, page - 1);
    else
        refresh_view(app); /* restore a valid value on bad input */
    gtk_widget_grab_focus(app->drawing_area);
}

static gboolean on_scroll(GtkWidget *widget, GdkEventScroll *event, gpointer data)
{
    App *app = (App *)data;
    (void)widget;
    if (event->state & GDK_CONTROL_MASK) {
        if (event->direction == GDK_SCROLL_UP)
            set_zoom(app, app->zoom * ZOOM_STEP);
        else if (event->direction == GDK_SCROLL_DOWN)
            set_zoom(app, app->zoom / ZOOM_STEP);
        return TRUE; /* consumed: don't also scroll the view */
    }
    return FALSE;
}

static gboolean on_key_press(GtkWidget *widget, GdkEventKey *event, gpointer data)
{
    App *app = (App *)data;
    (void)widget;
    gboolean ctrl = (event->state & GDK_CONTROL_MASK) != 0;

    /* Don't steal keystrokes while the user is typing in the page box. */
    if (gtk_widget_has_focus(app->page_entry) &&
        event->keyval != GDK_KEY_Escape)
        return FALSE;

    switch (event->keyval) {
    case GDK_KEY_o: case GDK_KEY_O:
        if (ctrl) { open_dialog(app); return TRUE; }
        break;
    case GDK_KEY_q: case GDK_KEY_Q:
        if (ctrl) { gtk_main_quit(); return TRUE; }
        break;
    case GDK_KEY_plus: case GDK_KEY_equal: case GDK_KEY_KP_Add:
        if (ctrl) { set_zoom(app, app->zoom * ZOOM_STEP); return TRUE; }
        break;
    case GDK_KEY_minus: case GDK_KEY_KP_Subtract:
        if (ctrl) { set_zoom(app, app->zoom / ZOOM_STEP); return TRUE; }
        break;
    case GDK_KEY_0: case GDK_KEY_KP_0:
        if (ctrl) { set_zoom(app, 1.0); return TRUE; }
        break;
    case GDK_KEY_Page_Down: case GDK_KEY_Right: case GDK_KEY_space:
        goto_page(app, app->current_page + 1);
        return TRUE;
    case GDK_KEY_Page_Up: case GDK_KEY_Left: case GDK_KEY_BackSpace:
        goto_page(app, app->current_page - 1);
        return TRUE;
    case GDK_KEY_Home:
        goto_page(app, 0);
        return TRUE;
    case GDK_KEY_End:
        goto_page(app, app->n_pages - 1);
        return TRUE;
    case GDK_KEY_Escape:
        gtk_widget_grab_focus(app->drawing_area);
        return TRUE;
    default:
        break;
    }
    return FALSE;
}

/* Lets the OS file manager hand us a file via "Open With". Also covers the
 * case where a filename was given on the command line: with
 * G_APPLICATION_HANDLES_OPEN, GTK fires "open" instead of "activate" then,
 * so the window may not exist yet. */
static void on_app_open(GApplication *gapp, GFile **files, gint n_files,
                        const gchar *hint, gpointer data)
{
    (void)hint;
    App *app = (App *)data;
    if (!app->window)
        build_ui(app, GTK_APPLICATION(gapp));
    gtk_window_present(GTK_WINDOW(app->window));
    if (n_files > 0) {
        gchar *path = g_file_get_path(files[0]);
        if (path) {
            open_path(app, path);
            g_free(path);
        }
    }
}

static GtkWidget *toolbar_button(const char *icon_name, const char *tooltip,
                                 GCallback cb, App *app)
{
    GtkWidget *btn = gtk_button_new_from_icon_name(icon_name, GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(btn, tooltip);
    g_signal_connect(btn, "clicked", cb, app);
    return btn;
}

/* ------------------------------------------------------------------ */
/* UI construction                                                     */
/* ------------------------------------------------------------------ */

static void build_ui(App *app, GtkApplication *gtk_app)
{
    app->window = gtk_application_window_new(gtk_app);
    gtk_window_set_title(GTK_WINDOW(app->window), "PDF Viewer");
    gtk_window_set_default_size(GTK_WINDOW(app->window), 900, 1000);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(app->window), vbox);

    /* --- toolbar --- */
    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(toolbar), 6);
    gtk_box_pack_start(GTK_BOX(vbox), toolbar, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(toolbar),
        toolbar_button("document-open", "Open (Ctrl+O)",
                       G_CALLBACK(on_open_clicked), app), FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(toolbar), gtk_separator_new(GTK_ORIENTATION_VERTICAL),
                       FALSE, FALSE, 4);

    gtk_box_pack_start(GTK_BOX(toolbar),
        toolbar_button("go-previous", "Previous page",
                       G_CALLBACK(on_prev_clicked), app), FALSE, FALSE, 0);

    app->page_entry = gtk_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(app->page_entry), 4);
    gtk_entry_set_alignment(GTK_ENTRY(app->page_entry), 0.5);
    g_signal_connect(app->page_entry, "activate",
                     G_CALLBACK(on_page_entry_activate), app);
    gtk_box_pack_start(GTK_BOX(toolbar), app->page_entry, FALSE, FALSE, 0);

    app->page_count_label = gtk_label_new("/ 0");
    gtk_box_pack_start(GTK_BOX(toolbar), app->page_count_label, FALSE, FALSE, 4);

    gtk_box_pack_start(GTK_BOX(toolbar),
        toolbar_button("go-next", "Next page",
                       G_CALLBACK(on_next_clicked), app), FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(toolbar), gtk_separator_new(GTK_ORIENTATION_VERTICAL),
                       FALSE, FALSE, 4);

    gtk_box_pack_start(GTK_BOX(toolbar),
        toolbar_button("zoom-out", "Zoom out (Ctrl+-)",
                       G_CALLBACK(on_zoom_out), app), FALSE, FALSE, 0);

    app->zoom_label = gtk_label_new("--");
    gtk_widget_set_size_request(app->zoom_label, 48, -1);
    gtk_box_pack_start(GTK_BOX(toolbar), app->zoom_label, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(toolbar),
        toolbar_button("zoom-in", "Zoom in (Ctrl++)",
                       G_CALLBACK(on_zoom_in), app), FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(toolbar),
        toolbar_button("zoom-original", "Reset zoom (Ctrl+0)",
                       G_CALLBACK(on_zoom_reset), app), FALSE, FALSE, 0);

    GtkWidget *fit_w_btn = gtk_button_new_with_label("Fit width");
    g_signal_connect(fit_w_btn, "clicked", G_CALLBACK(on_fit_width), app);
    gtk_box_pack_start(GTK_BOX(toolbar), fit_w_btn, FALSE, FALSE, 4);

    GtkWidget *fit_p_btn = gtk_button_new_with_label("Fit page");
    g_signal_connect(fit_p_btn, "clicked", G_CALLBACK(on_fit_page), app);
    gtk_box_pack_start(GTK_BOX(toolbar), fit_p_btn, FALSE, FALSE, 0);

    /* --- page view --- */
    app->scroller = gtk_scrolled_window_new(NULL, NULL);
    gtk_box_pack_start(GTK_BOX(vbox), app->scroller, TRUE, TRUE, 0);

    app->drawing_area = gtk_drawing_area_new();
    gtk_widget_set_halign(app->drawing_area, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(app->drawing_area, GTK_ALIGN_CENTER);
    gtk_widget_set_can_focus(app->drawing_area, TRUE);
    gtk_container_add(GTK_CONTAINER(app->scroller), app->drawing_area);
    g_signal_connect(app->drawing_area, "draw", G_CALLBACK(on_draw), app);

    gtk_widget_add_events(app->window, GDK_SCROLL_MASK);
    g_signal_connect(app->scroller, "scroll-event", G_CALLBACK(on_scroll), app);
    g_signal_connect(app->window, "key-press-event", G_CALLBACK(on_key_press), app);

    /* --- status bar --- */
    app->statusbar = gtk_statusbar_new();
    app->status_ctx = gtk_statusbar_get_context_id(GTK_STATUSBAR(app->statusbar), "main");
    gtk_box_pack_start(GTK_BOX(vbox), app->statusbar, FALSE, FALSE, 0);

    gtk_widget_show_all(app->window);
    set_status(app, "No document open - Ctrl+O to open a PDF");
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

static void on_activate(GtkApplication *gtk_app, gpointer data)
{
    App *app = (App *)data;
    if (!app->window)
        build_ui(app, gtk_app);
    gtk_window_present(GTK_WINDOW(app->window));
}

int main(int argc, char **argv)
{
    App app;
    memset(&app, 0, sizeof(app));
    app.zoom = 1.0;

    GtkApplication *gtk_app = gtk_application_new(
        "org.example.pdfviewer", G_APPLICATION_HANDLES_OPEN);

    g_signal_connect(gtk_app, "activate", G_CALLBACK(on_activate), &app);
    g_signal_connect(gtk_app, "open", G_CALLBACK(on_app_open), &app);

    int status = g_application_run(G_APPLICATION(gtk_app), argc, argv);

    if (app.doc)
        g_object_unref(app.doc);
    g_free(app.doc_path);
    g_object_unref(gtk_app);
    return status;
}
