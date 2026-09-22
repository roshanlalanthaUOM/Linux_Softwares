/*
 * mediaplayer.c - a simple audio/video player for Ubuntu
 *
 * Built with GTK3 (user interface) and GStreamer (playback engine).
 *
 * Features:
 *   - plays any audio/video format that GStreamer can decode
 *   - playlist (open many files, double-click to play, auto-advance)
 *   - seek bar with elapsed / total time
 *   - volume control
 *   - fullscreen mode
 *   - drag and drop files onto the window
 *   - files can also be given on the command line
 *
 * Keyboard shortcuts:
 *   Space       play / pause
 *   Left/Right  seek -5 s / +5 s
 *   Up/Down     volume +/- (when the playlist does not have focus)
 *   n / p       next / previous item
 *   s           stop
 *   f or F11    fullscreen (Esc leaves fullscreen)
 *   Ctrl+O      open files
 *   Delete      remove selected playlist items
 *
 * Build:  make
 */

#include <gtk/gtk.h>
#include <gst/gst.h>
#include <gdk/gdkkeysyms.h>

enum { COL_MARK, COL_TITLE, COL_URI, N_COLS };

typedef struct {
    GstElement   *playbin;
    GtkWidget    *window;
    GtkWidget    *side_box;
    GtkWidget    *controls;
    GtkWidget    *seek_scale;
    GtkWidget    *time_label;
    GtkWidget    *volume;
    GtkWidget    *play_image;
    GtkWidget    *tree;
    GtkListStore *store;
    gint          current;     /* index of the playlist item in use, -1 if none */
    gboolean      seeking;     /* TRUE while the user drags the seek bar        */
    gboolean      fullscreen;
    gint64        duration;    /* nanoseconds, 0 if unknown                     */
} Player;

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static gchar *format_time(gint64 ns)
{
    gint64 s = ns / GST_SECOND;
    if (s < 0) s = 0;
    gint h   = (gint)(s / 3600);
    gint m   = (gint)((s % 3600) / 60);
    gint sec = (gint)(s % 60);
    if (h > 0)
        return g_strdup_printf("%d:%02d:%02d", h, m, sec);
    return g_strdup_printf("%02d:%02d", m, sec);
}

static void update_time_label(Player *p, gint64 pos, gint64 dur)
{
    gchar *a = format_time(pos);
    gchar *b = format_time(dur);
    gchar *text = g_strdup_printf("%s / %s", a, b);
    gtk_label_set_text(GTK_LABEL(p->time_label), text);
    g_free(a);
    g_free(b);
    g_free(text);
}

static void reset_ui(Player *p)
{
    p->duration = 0;
    gtk_range_set_range(GTK_RANGE(p->seek_scale), 0, 100);
    gtk_range_set_value(GTK_RANGE(p->seek_scale), 0);
    update_time_label(p, 0, 0);
    gtk_image_set_from_icon_name(GTK_IMAGE(p->play_image),
                                 "media-playback-start-symbolic",
                                 GTK_ICON_SIZE_BUTTON);
}

static gint playlist_length(Player *p)
{
    return gtk_tree_model_iter_n_children(GTK_TREE_MODEL(p->store), NULL);
}

/* effective state: the pending state if a change is in progress */
static GstState effective_state(Player *p)
{
    GstState cur, pend;
    gst_element_get_state(p->playbin, &cur, &pend, 0);
    return (pend != GST_STATE_VOID_PENDING) ? pend : cur;
}

static gboolean is_idle(Player *p)
{
    return effective_state(p) <= GST_STATE_READY;
}

/* ------------------------------------------------------------------ */
/* playlist                                                            */
/* ------------------------------------------------------------------ */

static void set_mark(Player *p, gint idx)
{
    GtkTreeModel *m = GTK_TREE_MODEL(p->store);
    GtkTreeIter it;
    gint i = 0;
    gboolean ok = gtk_tree_model_get_iter_first(m, &it);
    while (ok) {
        gtk_list_store_set(p->store, &it, COL_MARK, (i == idx) ? "▶" : "", -1);
        i++;
        ok = gtk_tree_model_iter_next(m, &it);
    }
}

static gint find_mark(Player *p)
{
    GtkTreeModel *m = GTK_TREE_MODEL(p->store);
    GtkTreeIter it;
    gint i = 0;
    gboolean ok = gtk_tree_model_get_iter_first(m, &it);
    while (ok) {
        gchar *mk = NULL;
        gtk_tree_model_get(m, &it, COL_MARK, &mk, -1);
        gboolean marked = (mk != NULL && *mk != '\0');
        g_free(mk);
        if (marked) return i;
        i++;
        ok = gtk_tree_model_iter_next(m, &it);
    }
    return -1;
}

static void add_uri(Player *p, const gchar *uri)
{
    gchar *filename = g_filename_from_uri(uri, NULL, NULL);
    gchar *base     = filename ? g_path_get_basename(filename) : g_strdup(uri);
    gchar *title    = g_filename_display_name(base);
    GtkTreeIter it;

    gtk_list_store_append(p->store, &it);
    gtk_list_store_set(p->store, &it,
                       COL_MARK, "", COL_TITLE, title, COL_URI, uri, -1);
    g_free(filename);
    g_free(base);
    g_free(title);
}

static void play_index(Player *p, gint idx)
{
    GtkTreeIter it;
    GtkTreePath *path = gtk_tree_path_new_from_indices(idx, -1);
    GtkTreeModel *m = GTK_TREE_MODEL(p->store);

    if (!gtk_tree_model_get_iter(m, &it, path)) {
        gtk_tree_path_free(path);
        return;
    }
    gtk_tree_path_free(path);

    gchar *uri = NULL, *title = NULL;
    gtk_tree_model_get(m, &it, COL_URI, &uri, COL_TITLE, &title, -1);

    gst_element_set_state(p->playbin, GST_STATE_NULL);
    p->duration = 0;
    g_object_set(p->playbin, "uri", uri, NULL);
    gst_element_set_state(p->playbin, GST_STATE_PLAYING);

    p->current = idx;
    set_mark(p, idx);

    gchar *wt = g_strdup_printf("%s - Media Player", title);
    gtk_window_set_title(GTK_WINDOW(p->window), wt);
    g_free(wt);
    g_free(uri);
    g_free(title);
}

/* after adding items: start playing the first new one if nothing is active */
static void after_add(Player *p, gint first_new)
{
    if (playlist_length(p) > first_new && is_idle(p))
        play_index(p, first_new);
}

static void stop_playback(Player *p)
{
    gst_element_set_state(p->playbin, GST_STATE_NULL);
    reset_ui(p);
    gtk_window_set_title(GTK_WINDOW(p->window), "Media Player");
}

static void remove_selected(Player *p)
{
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(p->tree));
    GtkTreeModel *m = GTK_TREE_MODEL(p->store);
    GList *rows = gtk_tree_selection_get_selected_rows(sel, &m);
    GList *refs = NULL;

    for (GList *l = rows; l; l = l->next)
        refs = g_list_prepend(refs, gtk_tree_row_reference_new(m, l->data));

    for (GList *l = refs; l; l = l->next) {
        GtkTreePath *path = gtk_tree_row_reference_get_path(l->data);
        if (path) {
            GtkTreeIter it;
            if (gtk_tree_model_get_iter(m, &it, path))
                gtk_list_store_remove(p->store, &it);
            gtk_tree_path_free(path);
        }
        gtk_tree_row_reference_free(l->data);
    }
    g_list_free(refs);
    g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);

    if (p->current >= 0) {
        gint idx = find_mark(p);
        if (idx < 0) stop_playback(p);   /* the playing item was removed */
        p->current = idx;
    }
}

static void clear_playlist(Player *p)
{
    stop_playback(p);
    gtk_list_store_clear(p->store);
    p->current = -1;
}

/* ------------------------------------------------------------------ */
/* transport controls                                                  */
/* ------------------------------------------------------------------ */

static void open_dialog(Player *p)
{
    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        "Open media", GTK_WINDOW(p->window), GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Open",   GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(dlg), TRUE);

    GtkFileFilter *media = gtk_file_filter_new();
    gtk_file_filter_set_name(media, "Audio and video");
    gtk_file_filter_add_mime_type(media, "audio/*");
    gtk_file_filter_add_mime_type(media, "video/*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), media);

    GtkFileFilter *all = gtk_file_filter_new();
    gtk_file_filter_set_name(all, "All files");
    gtk_file_filter_add_pattern(all, "*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), all);

    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        GSList *uris = gtk_file_chooser_get_uris(GTK_FILE_CHOOSER(dlg));
        gint first_new = playlist_length(p);
        for (GSList *l = uris; l; l = l->next)
            add_uri(p, l->data);
        g_slist_free_full(uris, g_free);
        after_add(p, first_new);
    }
    gtk_widget_destroy(dlg);
}

static void toggle_play(Player *p)
{
    GstState st = effective_state(p);

    if (st == GST_STATE_PLAYING) {
        gst_element_set_state(p->playbin, GST_STATE_PAUSED);
    } else if (st == GST_STATE_PAUSED) {
        gst_element_set_state(p->playbin, GST_STATE_PLAYING);
    } else if (playlist_length(p) == 0) {
        open_dialog(p);
    } else {
        play_index(p, p->current >= 0 ? p->current : 0);
    }
}

static void play_next(Player *p)
{
    if (p->current + 1 < playlist_length(p))
        play_index(p, p->current + 1);
}

static void play_prev(Player *p)
{
    if (p->current > 0)
        play_index(p, p->current - 1);
}

static void seek_relative(Player *p, gint secs)
{
    gint64 pos = 0;
    if (!gst_element_query_position(p->playbin, GST_FORMAT_TIME, &pos))
        return;
    pos += (gint64)secs * GST_SECOND;
    if (pos < 0) pos = 0;
    if (p->duration > 0 && pos > p->duration) pos = p->duration;
    gst_element_seek_simple(p->playbin, GST_FORMAT_TIME,
                            GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE, pos);
}

static void volume_step(Player *p, gdouble delta)
{
    gdouble v = gtk_scale_button_get_value(GTK_SCALE_BUTTON(p->volume)) + delta;
    gtk_scale_button_set_value(GTK_SCALE_BUTTON(p->volume), CLAMP(v, 0.0, 1.0));
}

static void toggle_fullscreen(Player *p)
{
    p->fullscreen = !p->fullscreen;
    if (p->fullscreen)
        gtk_window_fullscreen(GTK_WINDOW(p->window));
    else
        gtk_window_unfullscreen(GTK_WINDOW(p->window));
    gtk_widget_set_visible(p->side_box, !p->fullscreen);
    gtk_widget_set_visible(p->controls, !p->fullscreen);
}

/* ------------------------------------------------------------------ */
/* GStreamer bus and periodic update                                   */
/* ------------------------------------------------------------------ */

static gboolean on_bus(GstBus *bus, GstMessage *msg, gpointer data)
{
    Player *p = data;
    (void)bus;

    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_EOS:
        if (p->current + 1 < playlist_length(p))
            play_index(p, p->current + 1);
        else
            stop_playback(p);
        break;

    case GST_MESSAGE_ERROR: {
        GError *err = NULL;
        gchar *dbg = NULL;
        gst_message_parse_error(msg, &err, &dbg);
        g_printerr("Playback error: %s\n%s\n", err->message, dbg ? dbg : "");
        stop_playback(p);

        GtkWidget *dlg = gtk_message_dialog_new(
            GTK_WINDOW(p->window), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR,
            GTK_BUTTONS_CLOSE, "Cannot play this file");
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dlg),
                                                 "%s", err->message);
        g_signal_connect_swapped(dlg, "response",
                                 G_CALLBACK(gtk_widget_destroy), dlg);
        gtk_widget_show(dlg);
        g_error_free(err);
        g_free(dbg);
        break;
    }

    case GST_MESSAGE_STATE_CHANGED:
        if (GST_MESSAGE_SRC(msg) == GST_OBJECT(p->playbin)) {
            GstState o, n, pd;
            gst_message_parse_state_changed(msg, &o, &n, &pd);
            gtk_image_set_from_icon_name(
                GTK_IMAGE(p->play_image),
                (n == GST_STATE_PLAYING) ? "media-playback-pause-symbolic"
                                         : "media-playback-start-symbolic",
                GTK_ICON_SIZE_BUTTON);
        }
        break;

    default:
        break;
    }
    return TRUE;
}

static gboolean on_tick(gpointer data)
{
    Player *p = data;
    GstState cur, pend;
    gint64 pos = 0, dur = 0;

    gst_element_get_state(p->playbin, &cur, &pend, 0);
    if (cur < GST_STATE_PAUSED)
        return G_SOURCE_CONTINUE;

    if (gst_element_query_duration(p->playbin, GST_FORMAT_TIME, &dur) && dur > 0
        && dur != p->duration) {
        p->duration = dur;
        gtk_range_set_range(GTK_RANGE(p->seek_scale), 0,
                            (gdouble)dur / GST_SECOND);
    }

    if (gst_element_query_position(p->playbin, GST_FORMAT_TIME, &pos)) {
        if (!p->seeking)
            gtk_range_set_value(GTK_RANGE(p->seek_scale),
                                (gdouble)pos / GST_SECOND);
        update_time_label(p, pos, p->duration);
    }
    return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* GTK signal handlers                                                 */
/* ------------------------------------------------------------------ */

static gboolean on_change_value(GtkRange *r, GtkScrollType s,
                                gdouble value, gpointer data)
{
    Player *p = data;
    (void)r; (void)s;
    if (value < 0) value = 0;
    gst_element_seek_simple(p->playbin, GST_FORMAT_TIME,
                            GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT,
                            (gint64)(value * GST_SECOND));
    return FALSE;   /* let the slider move */
}

static gboolean on_scale_press(GtkWidget *w, GdkEventButton *e, gpointer data)
{
    (void)w; (void)e;
    ((Player *)data)->seeking = TRUE;
    return FALSE;
}

static gboolean on_scale_release(GtkWidget *w, GdkEventButton *e, gpointer data)
{
    (void)w; (void)e;
    ((Player *)data)->seeking = FALSE;
    return FALSE;
}

static void on_volume(GtkScaleButton *b, gdouble value, gpointer data)
{
    (void)b;
    g_object_set(((Player *)data)->playbin, "volume", value, NULL);
}

static void on_row_activated(GtkTreeView *tv, GtkTreePath *path,
                             GtkTreeViewColumn *col, gpointer data)
{
    (void)tv; (void)col;
    play_index(data, gtk_tree_path_get_indices(path)[0]);
}

static gboolean on_video_click(GtkWidget *w, GdkEventButton *e, gpointer data)
{
    (void)w;
    if (e->type == GDK_2BUTTON_PRESS) {
        toggle_fullscreen(data);
        return TRUE;
    }
    return FALSE;
}

static gboolean on_key(GtkWidget *w, GdkEventKey *e, gpointer data)
{
    Player *p = data;
    GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(w));

    if ((e->state & GDK_CONTROL_MASK) && e->keyval == GDK_KEY_o) {
        open_dialog(p);
        return TRUE;
    }

    switch (e->keyval) {
    case GDK_KEY_space:  toggle_play(p);        return TRUE;
    case GDK_KEY_Left:   seek_relative(p, -5);  return TRUE;
    case GDK_KEY_Right:  seek_relative(p, 5);   return TRUE;
    case GDK_KEY_Up:
        if (focus == p->tree) return FALSE;     /* let the list scroll */
        volume_step(p, 0.05);
        return TRUE;
    case GDK_KEY_Down:
        if (focus == p->tree) return FALSE;
        volume_step(p, -0.05);
        return TRUE;
    case GDK_KEY_n:     play_next(p);           return TRUE;
    case GDK_KEY_p:     play_prev(p);           return TRUE;
    case GDK_KEY_s:     stop_playback(p);       return TRUE;
    case GDK_KEY_f:
    case GDK_KEY_F11:   toggle_fullscreen(p);   return TRUE;
    case GDK_KEY_Escape:
        if (p->fullscreen) toggle_fullscreen(p);
        return TRUE;
    case GDK_KEY_Delete: remove_selected(p);    return TRUE;
    default:             return FALSE;
    }
}

static void on_drag_data(GtkWidget *w, GdkDragContext *ctx, gint x, gint y,
                         GtkSelectionData *sel, guint info, guint time,
                         gpointer data)
{
    Player *p = data;
    (void)w; (void)x; (void)y; (void)info;

    gchar **uris = gtk_selection_data_get_uris(sel);
    gboolean ok = (uris != NULL);
    if (ok) {
        gint first_new = playlist_length(p);
        for (gint i = 0; uris[i]; i++)
            add_uri(p, uris[i]);
        g_strfreev(uris);
        after_add(p, first_new);
    }
    gtk_drag_finish(ctx, ok, FALSE, time);
}

/* small button callbacks */
static void cb_open(GtkButton *b, gpointer d)  { (void)b; open_dialog(d); }
static void cb_play(GtkButton *b, gpointer d)  { (void)b; toggle_play(d); }
static void cb_stop(GtkButton *b, gpointer d)  { (void)b; stop_playback(d); }
static void cb_next(GtkButton *b, gpointer d)  { (void)b; play_next(d); }
static void cb_prev(GtkButton *b, gpointer d)  { (void)b; play_prev(d); }
static void cb_full(GtkButton *b, gpointer d)  { (void)b; toggle_fullscreen(d); }
static void cb_remove(GtkButton *b, gpointer d){ (void)b; remove_selected(d); }
static void cb_clear(GtkButton *b, gpointer d) { (void)b; clear_playlist(d); }

static GtkWidget *icon_button(const gchar *icon, const gchar *tip,
                              GCallback cb, Player *p)
{
    GtkWidget *b = gtk_button_new_from_icon_name(icon, GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(b, tip);
    g_signal_connect(b, "clicked", cb, p);
    return b;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);
    gtk_init(&argc, &argv);

    Player p = {0};
    p.current = -1;

    /* --- GStreamer pipeline --- */
    p.playbin = gst_element_factory_make("playbin", "player");
    GstElement *gtksink = gst_element_factory_make("gtksink", "video-sink");
    if (!p.playbin || !gtksink) {
        g_printerr("Required GStreamer elements are missing.\n"
                   "Install them with:\n"
                   "  sudo apt install gstreamer1.0-plugins-base "
                   "gstreamer1.0-plugins-good gstreamer1.0-gtk3\n");
        return 1;
    }
    g_object_set(p.playbin, "video-sink", gtksink, "volume", 0.8, NULL);

    GstBus *bus = gst_element_get_bus(p.playbin);
    gst_bus_add_watch(bus, on_bus, &p);
    gst_object_unref(bus);

    /* --- window --- */
    p.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(p.window), "Media Player");
    gtk_window_set_default_size(GTK_WINDOW(p.window), 1000, 600);
    g_signal_connect(p.window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(p.window, "key-press-event", G_CALLBACK(on_key), &p);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(p.window), vbox);

    /* --- video area (inside an event box so we can catch double-clicks) --- */
    GtkWidget *video = NULL;
    g_object_get(gtksink, "widget", &video, NULL);
    gtk_widget_set_size_request(video, 480, 270);
    GtkWidget *ebox = gtk_event_box_new();
    gtk_container_add(GTK_CONTAINER(ebox), video);
    g_object_unref(video);
    g_signal_connect(ebox, "button-press-event", G_CALLBACK(on_video_click), &p);

    /* --- playlist --- */
    p.store = gtk_list_store_new(N_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    p.tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(p.store));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(p.tree), FALSE);
    gtk_tree_view_set_enable_search(GTK_TREE_VIEW(p.tree), FALSE);
    gtk_tree_selection_set_mode(
        gtk_tree_view_get_selection(GTK_TREE_VIEW(p.tree)), GTK_SELECTION_MULTIPLE);

    GtkCellRenderer *r_mark = gtk_cell_renderer_text_new();
    gtk_tree_view_insert_column_with_attributes(
        GTK_TREE_VIEW(p.tree), -1, "", r_mark, "text", COL_MARK, NULL);

    GtkCellRenderer *r_title = gtk_cell_renderer_text_new();
    g_object_set(r_title, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
    GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(
        "Playlist", r_title, "text", COL_TITLE, NULL);
    gtk_tree_view_column_set_expand(col, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(p.tree), col);
    g_signal_connect(p.tree, "row-activated", G_CALLBACK(on_row_activated), &p);

    GtkWidget *scroller = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(scroller), p.tree);
    gtk_widget_set_vexpand(scroller, TRUE);

    GtkWidget *list_buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *b_remove = gtk_button_new_with_label("Remove");
    GtkWidget *b_clear  = gtk_button_new_with_label("Clear");
    g_signal_connect(b_remove, "clicked", G_CALLBACK(cb_remove), &p);
    g_signal_connect(b_clear,  "clicked", G_CALLBACK(cb_clear),  &p);
    gtk_box_pack_start(GTK_BOX(list_buttons), b_remove, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(list_buttons), b_clear,  TRUE, TRUE, 0);

    p.side_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_size_request(p.side_box, 240, -1);
    gtk_container_set_border_width(GTK_CONTAINER(p.side_box), 6);
    gtk_box_pack_start(GTK_BOX(p.side_box), scroller, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(p.side_box), list_buttons, FALSE, FALSE, 0);

    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_pack1(GTK_PANED(paned), ebox, TRUE, FALSE);
    gtk_paned_pack2(GTK_PANED(paned), p.side_box, FALSE, FALSE);
    gtk_box_pack_start(GTK_BOX(vbox), paned, TRUE, TRUE, 0);

    /* --- control bar --- */
    p.controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(p.controls), 6);

    GtkWidget *b_open = icon_button("document-open-symbolic",
                                    "Open files (Ctrl+O)", G_CALLBACK(cb_open), &p);
    GtkWidget *b_prev = icon_button("media-skip-backward-symbolic",
                                    "Previous (p)", G_CALLBACK(cb_prev), &p);
    GtkWidget *b_stop = icon_button("media-playback-stop-symbolic",
                                    "Stop (s)", G_CALLBACK(cb_stop), &p);
    GtkWidget *b_next = icon_button("media-skip-forward-symbolic",
                                    "Next (n)", G_CALLBACK(cb_next), &p);
    GtkWidget *b_full = icon_button("view-fullscreen-symbolic",
                                    "Fullscreen (f)", G_CALLBACK(cb_full), &p);

    p.play_image = gtk_image_new_from_icon_name("media-playback-start-symbolic",
                                                GTK_ICON_SIZE_BUTTON);
    GtkWidget *b_play = gtk_button_new();
    gtk_container_add(GTK_CONTAINER(b_play), p.play_image);
    gtk_widget_set_tooltip_text(b_play, "Play / Pause (Space)");
    g_signal_connect(b_play, "clicked", G_CALLBACK(cb_play), &p);

    p.seek_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
    gtk_scale_set_draw_value(GTK_SCALE(p.seek_scale), FALSE);
    gtk_widget_set_hexpand(p.seek_scale, TRUE);
    g_signal_connect(p.seek_scale, "change-value", G_CALLBACK(on_change_value), &p);
    g_signal_connect(p.seek_scale, "button-press-event", G_CALLBACK(on_scale_press), &p);
    g_signal_connect(p.seek_scale, "button-release-event", G_CALLBACK(on_scale_release), &p);

    p.time_label = gtk_label_new("00:00 / 00:00");
    gtk_label_set_width_chars(GTK_LABEL(p.time_label), 15);

    p.volume = gtk_volume_button_new();
    gtk_scale_button_set_value(GTK_SCALE_BUTTON(p.volume), 0.8);
    g_signal_connect(p.volume, "value-changed", G_CALLBACK(on_volume), &p);

    gtk_box_pack_start(GTK_BOX(p.controls), b_open, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(p.controls), b_prev, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(p.controls), b_play, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(p.controls), b_stop, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(p.controls), b_next, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(p.controls), p.seek_scale, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(p.controls), p.time_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(p.controls), p.volume, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(p.controls), b_full, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), p.controls, FALSE, FALSE, 0);

    /* --- drag and drop --- */
    gtk_drag_dest_set(p.window, GTK_DEST_DEFAULT_ALL, NULL, 0, GDK_ACTION_COPY);
    gtk_drag_dest_add_uri_targets(p.window);
    g_signal_connect(p.window, "drag-data-received", G_CALLBACK(on_drag_data), &p);

    /* --- files given on the command line --- */
    for (gint i = 1; i < argc; i++) {
        GFile *f = g_file_new_for_commandline_arg(argv[i]);
        gchar *uri = g_file_get_uri(f);
        add_uri(&p, uri);
        g_free(uri);
        g_object_unref(f);
    }

    g_timeout_add(500, on_tick, &p);
    reset_ui(&p);
    gtk_widget_show_all(p.window);
    after_add(&p, 0);

    gtk_main();

    gst_element_set_state(p.playbin, GST_STATE_NULL);
    gst_object_unref(p.playbin);
    return 0;
}
