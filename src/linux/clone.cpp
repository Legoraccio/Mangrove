// The clone dialog: an address, or one of the repositories of the connected accounts picked in the list, the folder
// the clone goes in, and the clone's progress. The repository then opens in the window, in place of a start page.

#include "LinuxUI.h"

#include "clone.h"

namespace {

struct Clone {
    std::shared_ptr<CloneModel> model = std::make_shared<CloneModel>();
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
    GtkWidget* window = nullptr;  // where the repository opens (a reference)
    AdwDialog* dialog = nullptr;
    GtkWidget* entry = nullptr;
    GtkWidget* list = nullptr;
    GtkWidget* folder = nullptr;
    GtkWidget* choose = nullptr;
    GtkWidget* cancel = nullptr;
    GtkWidget* clone = nullptr;
    GtkWidget* spinner = nullptr;
    GtkWidget* status = nullptr;
    int listed = -1;       // the model's lists the dialog shows
    bool filling = false;  // the list is being refilled: its selection changes are not the user's

    ~Clone() {
        *alive = false;
        model->onChange = nullptr;
        if (window) g_object_unref(window);
    }
    void Fill();
    void Update();
};

GtkWidget* Note(const std::string& text, const char* cssClass) {
    GtkWidget* row = gtk_list_box_row_new();
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
    GtkWidget* label = MakeLabel(text, cssClass);
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_NONE);
    gtk_widget_set_margin_start(label, 12);
    gtk_widget_set_margin_end(label, 12);
    gtk_widget_set_margin_top(label, 10);
    gtk_widget_set_margin_bottom(label, 10);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), label);
    return row;
}

// The accounts and their repositories that match the words typed.
void Clone::Fill() {
    filling = true;
    GtkListBox* lb = GTK_LIST_BOX(list);
    gtk_list_box_remove_all(lb);
    const CloneModel& m = *model;
    if (IsGitAddress(m.text)) {
        gtk_list_box_append(lb, Note("Mangrove clones this address.", "dim-label"));
    } else if (m.sources.empty()) {
        gtk_list_box_append(lb, Note("Connect your GitHub, GitLab or other accounts in Settings \u203A Integrations to pick your "
                                     "repositories here, or paste an address above.", "dim-label"));
    }
    for (size_t s = 0; s < m.sources.size() && !IsGitAddress(m.text); ++s) {
        const CloneModel::Source& src = m.sources[s];
        GtkWidget* head = gtk_list_box_row_new();
        gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(head), FALSE);
        gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(head), FALSE);
        // an error goes below the account, so that the account keeps its room
        GtkWidget* box = gtk_box_new(src.error.empty() ? GTK_ORIENTATION_HORIZONTAL : GTK_ORIENTATION_VERTICAL, src.error.empty() ? 12 : 4);
        gtk_widget_set_margin_start(box, 12);
        gtk_widget_set_margin_end(box, 12);
        gtk_widget_set_margin_top(box, 8);
        gtk_widget_set_margin_bottom(box, 8);
        GtkWidget* who = MakeLabel(U8(std::wstring(InfoOf(src.account.forge).name) + L" \u00B7 " + src.account.login + L" \u00B7 " +
                                      src.account.Host()),
                                   "heading");
        gtk_widget_set_hexpand(who, TRUE);
        const std::wstring state = src.loading ? std::wstring(L"Loading\u2026") : !src.error.empty() ? src.error
                                                                                  : Plural(src.repos.size(), L"repository", L"repositories");
        GtkWidget* note = MakeLabel(U8(state), src.error.empty() ? "dim-label" : "error");
        gtk_label_set_wrap(GTK_LABEL(note), TRUE);
        gtk_label_set_ellipsize(GTK_LABEL(note), PANGO_ELLIPSIZE_NONE);
        gtk_label_set_xalign(GTK_LABEL(note), src.error.empty() ? 1 : 0);
        gtk_box_append(GTK_BOX(box), who);
        gtk_box_append(GTK_BOX(box), note);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(head), box);
        gtk_list_box_append(lb, head);
        for (const RemoteRepository& r : src.repos) {
            if (!m.Matches(r)) continue;
            GtkWidget* row = adw_action_row_new();
            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), Esc(U8(r.name)).c_str());
            adw_action_row_set_subtitle(ADW_ACTION_ROW(row), Esc(U8(r.description.empty() ? r.path : r.path + L" \u00B7 " + r.description)).c_str());
            adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(row), 1);
            if (r.isPrivate) {
                GtkWidget* lock = gtk_image_new_from_icon_name("changes-prevent-symbolic");
                gtk_widget_set_tooltip_text(lock, "Private");
                adw_action_row_add_suffix(ADW_ACTION_ROW(row), lock);
            }
            if (r.updated) adw_action_row_add_suffix(ADW_ACTION_ROW(row), MakeLabel(U8(FormatRelative(r.updated)), "dim-label"));
            gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
            g_object_set_data_full(G_OBJECT(row), "mg-url", g_strdup(U8(r.url).c_str()), g_free);
            gtk_list_box_append(lb, row);
            if (!m.picked.empty() && r.url == m.picked) gtk_list_box_select_row(lb, GTK_LIST_BOX_ROW(row));
        }
    }
    filling = false;
}

// The buttons, and what the clone does or did.
void Clone::Update() {
    const CloneModel& m = *model;
    gtk_widget_set_sensitive(clone, !m.running && m.Check().empty());
    gtk_button_set_label(GTK_BUTTON(clone), m.running ? "Cloning\u2026" : "Clone");
    for (GtkWidget* w : { entry, list, folder, choose, cancel }) gtk_widget_set_sensitive(w, !m.running);
    adw_dialog_set_can_close(dialog, !m.running);
    gtk_widget_set_visible(spinner, m.running);
    const std::wstring target = m.Target();
    std::wstring text;
    if (m.running) text = m.progress.empty() ? L"Cloning into " + target + L"\u2026" : m.progress;
    else if (!m.error.empty()) text = m.error;
    else if (!target.empty()) text = L"Clones into " + target;
    gtk_label_set_text(GTK_LABEL(status), U8(text).c_str());
    SetClass(status, "error", !m.running && !m.error.empty());
    SetClass(status, "dim-label", m.running || m.error.empty());
}

void Start(Clone* c) {
    if (c->model->Start()) return;
    c->Update();  // why it couldn't
}

struct ChooseState {
    Clone* clone;
    std::weak_ptr<bool> alive;
};

void Choose(Clone* c) {
    GtkFileDialog* d = gtk_file_dialog_new();
    gtk_file_dialog_set_title(d, "Where to Clone");
    gtk_file_dialog_set_accept_label(d, "Choose");
    if (!c->model->folder.empty()) {
        GFile* at = g_file_new_for_path(U8(c->model->folder).c_str());
        gtk_file_dialog_set_initial_folder(d, at);
        g_object_unref(at);
    }
    gtk_file_dialog_select_folder(
        d, c->window ? GTK_WINDOW(c->window) : nullptr, nullptr,
        [](GObject* src, GAsyncResult* res, gpointer data) {
            ChooseState* st = static_cast<ChooseState*>(data);
            if (GFile* f = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, nullptr)) {
                char* p = g_file_get_path(f);
                if (p && st->alive.lock()) gtk_editable_set_text(GTK_EDITABLE(st->clone->folder), p);  // "changed" updates the model
                g_free(p);
                g_object_unref(f);
            }
            delete st;
        },
        new ChooseState{ c, c->alive });
    g_object_unref(d);
}

}  // namespace

void ShowClone(GtkWidget* window) {
    Clone* c = new Clone;
    c->window = window ? GTK_WIDGET(g_object_ref(window)) : nullptr;
    AdwDialog* dlg = adw_dialog_new();
    c->dialog = dlg;
    adw_dialog_set_title(dlg, "Clone Repository");
    adw_dialog_set_content_width(dlg, 580);
    adw_dialog_set_content_height(dlg, 640);
    g_object_set_data_full(G_OBJECT(dlg), "mg-clone", c, [](gpointer p) { delete static_cast<Clone*>(p); });

    GtkWidget* view = adw_toolbar_view_new();
    GtkWidget* header = adw_header_bar_new();
    adw_header_bar_set_show_end_title_buttons(ADW_HEADER_BAR(header), FALSE);
    adw_header_bar_set_show_start_title_buttons(ADW_HEADER_BAR(header), FALSE);
    c->cancel = gtk_button_new_with_label("Cancel");
    c->clone = gtk_button_new_with_label("Clone");
    AddClass(c->clone, "suggested-action");
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), c->cancel);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), c->clone);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);

    GtkWidget* body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    for (auto margin : { gtk_widget_set_margin_start, gtk_widget_set_margin_end }) margin(body, 18);
    gtk_widget_set_margin_top(body, 6);
    gtk_widget_set_margin_bottom(body, 18);
    c->entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(c->entry), "An address, or words of one of your repositories");
    gtk_box_append(GTK_BOX(body), c->entry);
    c->list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(c->list), GTK_SELECTION_SINGLE);
    AddClass(c->list, "boxed-list");
    gtk_widget_set_valign(c->list, GTK_ALIGN_START);
    GtkWidget* scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), c->list);
    gtk_widget_set_vexpand(scroller, TRUE);
    gtk_box_append(GTK_BOX(body), scroller);
    GtkWidget* folderRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(folderRow), MakeLabel("Folder", "dim-label"));
    c->folder = gtk_entry_new();
    gtk_widget_set_hexpand(c->folder, TRUE);
    gtk_box_append(GTK_BOX(folderRow), c->folder);
    c->choose = gtk_button_new_with_label("Choose\u2026");
    gtk_box_append(GTK_BOX(folderRow), c->choose);
    gtk_box_append(GTK_BOX(body), folderRow);
    GtkWidget* statusRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    c->spinner = gtk_spinner_new();
    gtk_spinner_start(GTK_SPINNER(c->spinner));
    gtk_widget_set_valign(c->spinner, GTK_ALIGN_START);
    c->status = MakeLabel("", "dim-label");
    gtk_label_set_wrap(GTK_LABEL(c->status), TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(c->status), PANGO_ELLIPSIZE_NONE);
    gtk_label_set_selectable(GTK_LABEL(c->status), TRUE);
    gtk_widget_set_hexpand(c->status, TRUE);
    gtk_box_append(GTK_BOX(statusRow), c->spinner);
    gtk_box_append(GTK_BOX(statusRow), c->status);
    gtk_box_append(GTK_BOX(body), statusRow);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), body);
    adw_dialog_set_child(dlg, view);

    // the address or the words, the pick, the folder, the clone
    g_signal_connect(c->entry, "changed", G_CALLBACK(+[](GtkEditable* e, gpointer d) {
        Clone* c = static_cast<Clone*>(d);
        CloneModel& m = *c->model;
        m.text = Widen(gtk_editable_get_text(e));
        m.picked.clear();  // the list changed under it
        m.error.clear();
        c->Fill();
        c->Update();
    }), c);
    g_signal_connect(c->entry, "activate", G_CALLBACK(+[](GtkEntry*, gpointer d) { Start(static_cast<Clone*>(d)); }), c);
    g_signal_connect(c->list, "row-selected", G_CALLBACK(+[](GtkListBox*, GtkListBoxRow* row, gpointer d) {
        Clone* c = static_cast<Clone*>(d);
        const char* url = row ? static_cast<const char*>(g_object_get_data(G_OBJECT(row), "mg-url")) : nullptr;
        if (c->filling || !url) return;
        c->model->picked = Widen(url);
        c->model->error.clear();
        c->Update();
    }), c);
    g_signal_connect(c->list, "row-activated", G_CALLBACK(+[](GtkListBox*, GtkListBoxRow* row, gpointer d) {
        Clone* c = static_cast<Clone*>(d);
        if (const char* url = static_cast<const char*>(g_object_get_data(G_OBJECT(row), "mg-url"))) {
            c->model->picked = Widen(url);
            Start(c);
        }
    }), c);
    g_signal_connect(c->folder, "changed", G_CALLBACK(+[](GtkEditable* e, gpointer d) {
        Clone* c = static_cast<Clone*>(d);
        c->model->folder = TrimW(Widen(gtk_editable_get_text(e)));
        c->model->error.clear();
        c->Update();
    }), c);
    g_signal_connect(c->folder, "activate", G_CALLBACK(+[](GtkEntry*, gpointer d) { Start(static_cast<Clone*>(d)); }), c);
    g_signal_connect(c->choose, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) { Choose(static_cast<Clone*>(d)); }), c);
    g_signal_connect(c->clone, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) { Start(static_cast<Clone*>(d)); }), c);
    g_signal_connect(c->cancel, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) { adw_dialog_close(static_cast<Clone*>(d)->dialog); }), c);

    std::weak_ptr<bool> alive = c->alive;
    c->model->onChange = [c, alive] {
        if (!alive.lock()) return;
        if (!c->model->cloned.empty()) {  // done: the dialog goes, once this call returned, and the repository opens
            const std::wstring path = c->model->cloned;
            GtkWidget* window = c->window ? GTK_WIDGET(g_object_ref(c->window)) : nullptr;
            Later([c, alive, path, window] {
                if (alive.lock()) adw_dialog_force_close(c->dialog);
                OpenRepository(path, window);
                if (window) g_object_unref(window);
            });
            return;
        }
        if (c->listed != c->model->listed) {
            c->listed = c->model->listed;
            c->Fill();
        }
        c->Update();
    };
    c->model->folder = DefaultCloneFolder();
    gtk_editable_set_text(GTK_EDITABLE(c->folder), U8(c->model->folder).c_str());
    c->model->Load();
    c->listed = c->model->listed;
    c->Fill();
    c->Update();
    adw_dialog_present(dlg, window);
    adw_dialog_set_focus(dlg, c->entry);
}
