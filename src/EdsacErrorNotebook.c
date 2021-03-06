/*
 * Copyright 2017
 * GPL3 Licensed
 * EdsacErrorNotebook.c
 * GObject Class inheriting from GtkNotebook implementing the tabbed list of errors
 */

// includes
#include "config.h"
#include "EdsacErrorNotebook.h"
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include "sql.h"
#include "ui.h"

// declarations

// context for an open tab
typedef struct _LinkyTextBuffer {
    Clickable description;  // information about what this is a list of
    GtkTextBuffer *buffer;  // the text buffer
    GSList *g_string_list;  // stuff to call g_string_free(., TRUE) on when we clear buffer
    GSList *clickables;     // Clickables *within the text buffer* we need to free
    gint page_id;           // the gtknotebook page id
    GString *title;         // The string for the tab's title
} LinkyBuffer;

// private object data
typedef struct _EdsacErrorNotebookPrivate {
    GSList *open_tabs_list; // list of open tabs (LinkyBuffers)
} EdsacErrorNotebookPrivate;

static gpointer edsac_error_notebook_parent_class = NULL;
#define EDSAC_ERROR_NOTEBOOK_GET_PRIVATE(_o) (G_TYPE_INSTANCE_GET_PRIVATE((_o), EDSAC_TYPE_ERROR_NOTEBOOK, EdsacErrorNotebookPrivate))

/**** local function declarations ****/
// My Structures
static bool clickable_compare(const Clickable *a, const Clickable *b);
static gint open_tabs_list_compare_by_id(gconstpointer a, gconstpointer b);
static gint open_tabs_list_compare_by_desc(gconstpointer a, gconstpointer b);
static void open_tabs_list_dec_id(gpointer data, gpointer unused);
static gint open_tabs_list_search_by_id(gconstpointer result, gconstpointer id);
static LinkyBuffer *new_linky_buffer(const Clickable *description);
static void append_linky_text_buffer(LinkyBuffer *linky_buffer, SearchResult *data);
static void free_g_string(gpointer g_string);
static void free_linky_buffer(LinkyBuffer *linky_buffer);
static void add_link(size_t start_pos, size_t end_pos, GtkTextBuffer *buffer, Clickable* data);
static void update_tab(gpointer data, gpointer unused);
static notebook_page_id_t add_new_page_to_notebook(EdsacErrorNotebook *self, const Clickable *data);
static void close_tab(EdsacErrorNotebook *self, GSList *tab_in_list);

// GTK
static GtkWidget *new_text_view(void);
static GtkWidget *put_in_scroll(GtkWidget *thing);
static GtkWidget *tab_label(const char *msg, GtkWidget *contents);
static GtkWidget *get_parent(const GtkWidget *child);

// Signal Handlers
static void close_button_handler(GtkWidget *button, GdkEvent *event, GtkWidget *contents);
static void link_clicked(const GtkTextTag *tag, const GtkTextView *parent, const GdkEvent *event, const GtkTextIter *iter, Clickable *data);
static void desc_clicked(const GtkTextTag *tag, const GtkTextView *parent, const GdkEvent *event, const GtkTextIter *iter, const gpointer error_id);
static void disable_click(const uintptr_t id);

/**** Public Methods ****/
// update data to be in line with the database
void edsac_error_notebook_update(EdsacErrorNotebook *self) {
    g_slist_foreach(self->priv->open_tabs_list, update_tab, NULL);
}

// get the error count for the currently displayed page
int edsac_error_notebook_get_error_count(EdsacErrorNotebook *self) {
    assert(NULL != self);

    const gint current_page = gtk_notebook_get_current_page(&self->parent_instance);
    if (-1 == current_page) {
        puts("bad current page");
        return -1;
    }

    // look up the current page
    GSList *result = g_slist_find_custom(self->priv->open_tabs_list, (gconstpointer) &current_page, open_tabs_list_search_by_id);

    if (NULL == result) {
        puts("current page not found");
        return -1;
    } 

    LinkyBuffer *linky_buffer = (LinkyBuffer *) result->data;
    return count_clickable(&linky_buffer->description);
}

void edsac_error_notebook_show_page(EdsacErrorNotebook *self, const Clickable *data) {
    // check if the tab we want is already open
    LinkyBuffer data_desc;
    data_desc.page_id = -1;
    memcpy(&data_desc.description, data, sizeof(data_desc.description));
    GSList *found = g_slist_find_custom(self->priv->open_tabs_list, (gconstpointer) &data_desc, open_tabs_list_compare_by_desc);
    if (NULL != found) {
        LinkyBuffer *tab = (LinkyBuffer *) found->data;
        assert(NULL != tab);

        gtk_notebook_set_current_page(GTK_NOTEBOOK(self), tab->page_id); // I don't think there is any reason to re-lock for this?
        // assumes that a and b are locked by the caller
        return;
    }

    // our tab is not open so we need to make a new one...
    add_new_page_to_notebook(self, data);
}

void edsac_error_notebook_close_node(EdsacErrorNotebook *self, const unsigned int rack_no, const unsigned int chassis_no) {
    if (NULL == self)
        return;

    GSList *item = self->priv->open_tabs_list;
    while (NULL != item) {
        LinkyBuffer *linky_buffer = item->data;
        if (NULL == linky_buffer)
            return;

        if (linky_buffer->description.rack_num == rack_no) {
            if (linky_buffer->description.chassis_num == chassis_no) {
                // this tab needs closing
                close_tab(self, item);
                // the list has now changed so search back from the beginning (strictly we only need to go back by one but in a very short singley linked list this is much easier to do)
                item = self->priv->open_tabs_list;
            } else {
                item = item->next;
            }
        } else {
            item = item->next;
        }

    }
}

// add a new page to the notebook
static notebook_page_id_t add_new_page_to_notebook(EdsacErrorNotebook *self, const Clickable *data) {
    if ((NULL == self) || (NULL == data)) {
        return NULL;
    }

    GtkNotebook *notebook = &self->parent_instance;

    // LinkyBuffer to describe the notebook
    LinkyBuffer *linky_buffer = new_linky_buffer(data);
    assert(NULL != linky_buffer);

    // Heading for the new tab
    linky_buffer->title = g_string_new(NULL);
    assert(NULL != linky_buffer->title);

    switch(data->type) {
        case ALL:
            g_string_printf(linky_buffer->title, "All");
            break;
        case RACK:
            g_string_printf(linky_buffer->title, "Rack %i", data->rack_num);
            break;
        case CHASSIS:
            g_string_printf(linky_buffer->title, "Rack %i, Chassis: %i", data->rack_num, data->chassis_num);
            break;
        case VALVE:
            g_string_printf(linky_buffer->title, "Rack %i, Chassis %i, Valve: %i", data->rack_num, data->chassis_num, data->valve_num);
            break;
        default:
            g_string_printf(linky_buffer->title, "(Unknown)");
    }

    GtkWidget *msg = new_text_view(); 
    assert(NULL != msg);

    gtk_text_view_set_buffer(GTK_TEXT_VIEW(msg), linky_buffer->buffer);

    GtkWidget *scroll = put_in_scroll(msg);
    assert(NULL != scroll);

    gint index = gtk_notebook_append_page(notebook, scroll, tab_label(linky_buffer->title->str, scroll));
    assert(-1 != index);
    linky_buffer->page_id = index;

    // set child property
    GtkWidget *child = gtk_notebook_get_nth_page(notebook, index);
    GValue value;
    memset(&value, 0, sizeof(value));
    g_value_init(&value, G_TYPE_BOOLEAN);
    g_value_set_boolean(&value, TRUE);
    gtk_container_child_set_property(GTK_CONTAINER(notebook), child, "tab-expand", &value);

    // add the new tab to our open tabs list
    self->priv->open_tabs_list = g_slist_insert_sorted(self->priv->open_tabs_list, linky_buffer, open_tabs_list_compare_by_id);

    // update the new page
    update_tab((gpointer) linky_buffer, NULL);

    // show the new page
    GtkWidget *page = gtk_notebook_get_nth_page(notebook, index);
    gtk_widget_show_all(page);

    // change to the new page
    gtk_notebook_set_current_page(notebook, index);

    return linky_buffer;
}

static void close_tab(EdsacErrorNotebook *self, GSList *tab_in_list) {
    assert(NULL != tab_in_list);

    LinkyBuffer *tab_page_desc = tab_in_list->data;
    const gint page_id = tab_page_desc->page_id;

    // any tabs after this one need to have their id adjusted down by 1 (assuming list sorted by id)
    // open_tabs_list_dec_id does locking
    g_slist_foreach(tab_in_list, open_tabs_list_dec_id, NULL);

    // remove tab from the list
    self->priv->open_tabs_list = g_slist_delete_link(self->priv->open_tabs_list, tab_in_list);

    // remove page from notebook
    gtk_notebook_remove_page(GTK_NOTEBOOK(self), page_id);

    // free the linky buffer
    free_linky_buffer(tab_page_desc);

    // close the window if the last page is closed
    if (0 == gtk_notebook_get_n_pages(GTK_NOTEBOOK(self))) {
        GtkWidget *toplevel = gtk_widget_get_toplevel(GTK_WIDGET(self));
        if (gtk_widget_is_toplevel(toplevel)) { // docs recommend this extra check https://developer.gnome.org/gtk3/stable/GtkWidget.html#gtk-widget-get-toplevel
            GtkWindow *window = GTK_WINDOW(toplevel);
            assert(NULL != window);

            gtk_window_close(window);
        } else {
            perror("Could not find the top level widget");
            exit(EXIT_FAILURE);
        }
    }
}




/*** stuff to do with internal structures ***/

// set default argument so that we can iterate through teh list more easily
static void free_g_string(gpointer g_string) {
    g_string_free((GString *) g_string, TRUE);
}

// free a LinkyBuffer
static void free_linky_buffer(LinkyBuffer *linky_buffer) {
    assert(NULL != linky_buffer);


    g_slist_free_full(linky_buffer->g_string_list, free_g_string);
    g_slist_free_full(linky_buffer->clickables, g_free); // invalid free

    free_g_string(linky_buffer->title);

    g_free(linky_buffer);
}

// are two Clickables equal? Ignores undefined fields.
static bool clickable_compare(const Clickable *a, const Clickable *b) {
    assert(NULL != a);
    assert(NULL != b);

    if (a->type != b->type) {
        return false;
    }

    if (ALL == a->type) {
        return true;
    }

    const bool rack_num = (a->rack_num == b->rack_num);
    if ((RACK == a->type) && rack_num) {
        return true;
    }

    const bool chassis_num = (a->chassis_num == b->chassis_num);
    if ((CHASSIS == a->type) && rack_num && chassis_num) {
        return true;
    }

    const bool valve_num = (a->chassis_num == b->chassis_num);
    if ((VALVE == a->type) && rack_num && chassis_num && valve_num) {
        return true;
    }

    return false;
}

// open tabs list compare func for searching by id
// implements GCompareFunc: 0 is equal
static gint open_tabs_list_compare_by_id(gconstpointer a, gconstpointer b) {
    assert(NULL != a);
    assert(NULL != b);

    LinkyBuffer *A = (LinkyBuffer *) a;
    LinkyBuffer *B = (LinkyBuffer *) b;

    const gint ret = A->page_id - B->page_id;

    return ret;
}

// decrements the page id of something on the open tabs list. Prototype to match glib foreach
static void open_tabs_list_dec_id(gpointer data, __attribute__((unused)) gpointer unused) {
    assert(NULL != data);

    LinkyBuffer *tab_page_desc = (LinkyBuffer *) data;
 
    tab_page_desc->page_id -= 1;
}

// returns 0 if linky_buffer matches the id
static gint open_tabs_list_search_by_id(gconstpointer result, gconstpointer id) {
    assert(NULL != id);
    assert(NULL != result);

    const LinkyBuffer *linky_buffer = (LinkyBuffer *) result;
    const gint page_id = *((gint *) id);
    
    if (linky_buffer->page_id == page_id) {
        return 0;
    }
    
    return 1;
}

// creates a new LinkyBuffer
static LinkyBuffer *new_linky_buffer(const Clickable *desc) {
    LinkyBuffer *linky_buffer = malloc(sizeof(LinkyBuffer));
    assert(NULL != linky_buffer);

    // default values
    linky_buffer->g_string_list = NULL;
    linky_buffer->clickables = NULL;
    linky_buffer->page_id = -1;
    linky_buffer->buffer = gtk_text_buffer_new(NULL);
    assert(NULL != linky_buffer->buffer);

    // set description
    memcpy(&linky_buffer->description, desc, sizeof(linky_buffer->description));
    
    // make the text buffer un-editable
    GtkTextTag *un_editable_tag = gtk_text_buffer_create_tag(linky_buffer->buffer, "un_editable", "editable", FALSE, "editable-set", TRUE, NULL);
    assert(NULL != un_editable_tag);
    GtkTextIter start; 
    GtkTextIter end;
    gtk_text_buffer_get_start_iter(linky_buffer->buffer, &start);
    gtk_text_buffer_get_end_iter(linky_buffer->buffer, &end);
    gtk_text_buffer_apply_tag(linky_buffer->buffer, un_editable_tag, &start, &end);

    return linky_buffer;
}

// appends the specified error message to the linky buffer
// negative valve_no to not specify it
// assumes that linky_buffer is already locked
static void append_linky_text_buffer(LinkyBuffer *linky_buffer, SearchResult *data) {
    // generate message string
    GString *message = g_string_new(NULL);
    assert(NULL != message);
    linky_buffer->g_string_list = g_slist_prepend(linky_buffer->g_string_list, message); // so we know to free message

    GtkTextIter buffer_end;
    gtk_text_buffer_get_end_iter(linky_buffer->buffer, &buffer_end);
    const gsize offset = (gsize) gtk_text_iter_get_offset(&buffer_end);

    g_string_printf(message, "Rack: %i, ", data->rack_no);
    const gsize rack_start = offset;
    const gsize rack_end = offset + message->len - 2; // comma, space 
    const gsize chassis_start = offset + message->len; 

    g_string_append_printf(message, "Chassis: %i, ", data->chassis_no);
    const gsize chassis_end = offset + message->len - 2; // comma, space 
    const gsize valve_start = offset + message->len; 

    if (data->valve_no >= 0) {
        g_string_append_printf(message, "Valve: %i: ", data->valve_no);
    }
    const gsize valve_end = offset + message->len - 2; // colon, space. (this is unused when valve_no < 0 so it remains valid)
    
    g_string_append_printf(message, "%s\n", data->message);
    
    gtk_text_buffer_insert(linky_buffer->buffer, &buffer_end, message->str, (gint) message->len);

    // clickable objects to describe the links in this row
    Clickable *rack_data = malloc(sizeof(Clickable));
    assert(NULL != rack_data);
    rack_data->type = RACK;
    rack_data->rack_num = data->rack_no;

    Clickable *chassis_data = malloc(sizeof(Clickable));
    assert(NULL != chassis_data);
    chassis_data->type = CHASSIS;
    chassis_data->rack_num = data->rack_no;
    chassis_data->chassis_num = data->chassis_no;

    Clickable *valve_data = NULL;
    if (data->valve_no >= 0) {
        valve_data = malloc(sizeof(Clickable));
        assert(NULL != valve_data);
        valve_data->type = VALVE;
        valve_data->rack_num = data->rack_no;
        valve_data->chassis_num = data->chassis_no;
        valve_data->valve_num = data->valve_no; 
    }

    // so we know to free them
    linky_buffer->clickables = g_slist_prepend(linky_buffer->clickables, rack_data);
    linky_buffer->clickables = g_slist_prepend(linky_buffer->clickables, chassis_data);
    if (data->valve_no >= 0) {
        linky_buffer->clickables = g_slist_prepend(linky_buffer->clickables, valve_data);
    }
    
    // tag over the description for right-clicking
    GtkTextIter start;
    GtkTextIter end;
    gtk_text_buffer_get_iter_at_offset(linky_buffer->buffer, &start, (gint) valve_end);
    gtk_text_buffer_get_end_iter(linky_buffer->buffer, &end);

    GtkTextTag *description = NULL;
    // grey out disabled items
    if (!data->enabled) {
        description = gtk_text_buffer_create_tag(linky_buffer->buffer, NULL,
            "foreground", "grey", NULL);
    } else {
        description = gtk_text_buffer_create_tag(linky_buffer->buffer, NULL, NULL);
    }
    g_signal_connect(G_OBJECT(description), "event", G_CALLBACK(desc_clicked), (gpointer) ((uintptr_t) data->id));
    gtk_text_buffer_apply_tag(linky_buffer->buffer, description, &start, &end);

    // links to other pages
    add_link(rack_start, rack_end, linky_buffer->buffer, rack_data);
    add_link(chassis_start, chassis_end, linky_buffer->buffer, chassis_data);
    if (data->valve_no >= 0) {
        add_link(valve_start, valve_end, linky_buffer->buffer, valve_data);
    }
}

// adds a hyperlink to a text buffer
static void add_link(size_t start_pos, size_t end_pos, GtkTextBuffer *buffer, Clickable *data) {
    GtkTextTag *url = gtk_text_buffer_create_tag(buffer, NULL,
        "underline", PANGO_UNDERLINE_SINGLE, "underline-set", TRUE,
        "foreground", "blue", NULL);

    // make it clickable
    // event is the only signal allowed on a tag
    g_signal_connect(G_OBJECT(url), "event", G_CALLBACK(link_clicked), (gpointer) data);
    
    GtkTextIter start;
    GtkTextIter end;
    gtk_text_buffer_get_iter_at_offset(buffer, &start, (gint) start_pos);
    gtk_text_buffer_get_iter_at_offset(buffer, &end, (gint) end_pos);
    gtk_text_buffer_apply_tag(buffer, url, &start, &end);
}

// open tabs list compare func for searching by description
// implements GCompareFunc: 0 is equal
static gint open_tabs_list_compare_by_desc(gconstpointer a, gconstpointer b) {
    assert(NULL != a);
    assert(NULL != b);

    LinkyBuffer *A = (LinkyBuffer *) a;
    LinkyBuffer *B = (LinkyBuffer *) b;

    gint ret = 1;

    if (clickable_compare(&A->description, &B->description)) {
        ret = 0;
    } 

    return ret;
}

static void insert_search_result(gpointer data, gpointer user_data) {
    if ((NULL == data) || (NULL == user_data)) {
        return;
    }

    SearchResult *res = (SearchResult *) data;
    LinkyBuffer *linky_buffer = (LinkyBuffer *) user_data;

    append_linky_text_buffer(linky_buffer, res);
}

static void update_tab(gpointer data, __attribute__((unused)) gpointer unused) {
    assert(NULL != data);
    LinkyBuffer *linky_buffer = (LinkyBuffer *) data;

    // query the database
    GList *results = search_clickable(&linky_buffer->description);

    // clear stuff already in the buffer
    GtkTextIter start;
    gtk_text_buffer_get_start_iter(linky_buffer->buffer, &start);
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(linky_buffer->buffer, &end);
    
    gtk_text_buffer_delete(linky_buffer->buffer, &start, &end);

    g_slist_free_full(linky_buffer->g_string_list, free_g_string);
    linky_buffer->g_string_list = NULL;
    g_slist_free_full(linky_buffer->clickables, g_free);
    linky_buffer->clickables = NULL;

    g_list_foreach(results, insert_search_result, (gpointer) linky_buffer);
}



/**** GTK Signal Handlers ****/
// handler for when a link text tag is clicked
static void link_clicked(__attribute__((unused)) const GtkTextTag *tag, const GtkTextView *parent, const GdkEvent *event,
                    __attribute__((unused)) const GtkTextIter *iter, Clickable *data) {
    assert(NULL != event);
    // work out if the event was a clicked
    GdkEventButton *event_btn = (GdkEventButton *) event;
    if (event->type == GDK_BUTTON_PRESS && event_btn->button == 1) { // left click
        assert(NULL != data);
        assert(NULL != parent);
        // work up the tree to the notebook
        GtkWidget *text_view = GTK_WIDGET(parent);
        GtkWidget *scrolled_window = get_parent(text_view);
        EdsacErrorNotebook *notebook = EDSAC_ERROR_NOTEBOOK(get_parent(scrolled_window));

        edsac_error_notebook_show_page(notebook, data);
    }
}

static void disable_click(const uintptr_t id) {
    error_toggle_disabled(id);
    gui_update(NULL);
}

// handler for when a description is clicked
static void desc_clicked(__attribute((unused)) const GtkTextTag *tag , __attribute__((unused)) const GtkTextView *parent, const GdkEvent *event,
        __attribute__((unused)) const GtkTextIter *iter, const gpointer error_id) {
    assert(NULL != event);

    // was it a click?
    GdkEventButton *event_btn = (GdkEventButton *) event;
    if (event->type == GDK_BUTTON_PRESS && event_btn->button == 1) { // left click. Not using right click because TextView already has a context menu that we can't remove
        // show right-click menu
        GtkWidget *menu = gtk_menu_new();
        assert(NULL != menu);

        GtkWidget *menu_item = gtk_menu_item_new_with_label("Toggle Disabled");
        assert(NULL != menu_item);

        g_signal_connect_swapped(G_OBJECT(menu_item), "activate", G_CALLBACK(disable_click), error_id);

        gtk_menu_shell_append(GTK_MENU_SHELL(menu), menu_item);
        gtk_widget_show_all(menu);
        gtk_menu_popup_at_pointer(GTK_MENU(menu), event); 
    }
}

// handler for the close button on tab labels
static void close_button_handler(GtkWidget *button, __attribute__((unused)) GdkEvent *event, GtkWidget *contents) {
    assert(NULL != button);
    assert(NULL != contents);

    // get the notebook
    GtkWidget *grid = GTK_WIDGET(get_parent(button));
    GtkWidget *frame = GTK_WIDGET(get_parent(grid));
    EdsacErrorNotebook *notebook = EDSAC_ERROR_NOTEBOOK(get_parent(frame));
    assert(NULL != notebook);

    const gint page_num = gtk_notebook_page_num(GTK_NOTEBOOK(notebook), contents);
    if (-1 == page_num)
        return;

    // try to find this tab in the open tabs list
    LinkyBuffer example;
    example.page_id = page_num;

    GSList *result = g_slist_find_custom(notebook->priv->open_tabs_list, (gconstpointer) &example, open_tabs_list_compare_by_id);
    if (NULL == result) {
        g_print("Closing a tab which was not open! id=%i\n", page_num);
        return;
    } else {
        close_tab(notebook, result);
    } 
}



/**** GTK stuff ****/
// new gtk text view without a visale cursor
static GtkWidget *new_text_view(void) {
    GtkWidget *text_view = gtk_text_view_new_with_buffer(NULL);
    
    // get rid of the cursor
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(text_view), FALSE);

    return text_view;
}

// puts thing into a scrolled window
static GtkWidget *put_in_scroll(GtkWidget *thing) {
    assert(NULL != thing);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    assert(NULL != scroll);

    gtk_container_add(GTK_CONTAINER(scroll), thing);

    return scroll;
}

// creates the widget used to label a tab
static GtkWidget *tab_label(const char *msg, GtkWidget *contents) {
    assert(NULL != msg);
    assert(NULL != contents);

    // text - using a status bar so that the style matches
    //GtkWidget *text = gtk_label_new(msg);
    GtkWidget *text = gtk_statusbar_new();
    gtk_statusbar_push(GTK_STATUSBAR(text), 0, msg);

    // close button
    GtkWidget *close = gtk_button_new_from_icon_name("window-close", GTK_ICON_SIZE_BUTTON);
    g_signal_connect(G_OBJECT(close), "button-press-event", (GCallback) close_button_handler, contents);

    // container
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 1);
    gtk_box_pack_start(GTK_BOX(box), text, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), close, FALSE, FALSE, 0);

    // put it in a frame so we get boarders
    GtkWidget *frame = gtk_frame_new(NULL);
    gtk_container_add(GTK_CONTAINER(frame), box);

    gtk_widget_show_all(frame);
    return GTK_WIDGET(frame);
} 

// get the Gtk Parent from the parent property
static GtkWidget *get_parent(const GtkWidget *child) {
    assert(NULL != child);

    GValue parent_g_value;
    memset(&parent_g_value, '\0', sizeof(parent_g_value));
    g_value_init(&parent_g_value, GTK_TYPE_CONTAINER);

    g_object_get_property(G_OBJECT(child), "parent", &parent_g_value);

    return GTK_WIDGET(g_value_get_object(&parent_g_value));
}



/**** internal GObject stuff ****/
EdsacErrorNotebook *edsac_error_notebook_construct(GType object_type) {
    EdsacErrorNotebook *self = (EdsacErrorNotebook *) g_object_new(object_type, NULL);
    return self;
}

// DESTROY PRIVATE MEMBER DATA HERE
static void edsac_error_notebook_finalize(GObject *obj) {
    EdsacErrorNotebook *self = EDSAC_ERROR_NOTEBOOK(obj);

    g_slist_free_full(self->priv->open_tabs_list, (GDestroyNotify) free_linky_buffer);

    G_OBJECT_CLASS(edsac_error_notebook_parent_class)->finalize(obj);
}

static void edsac_error_notebook_class_init(EdsacErrorNotebookClass *class) {
    edsac_error_notebook_parent_class = g_type_class_peek_parent(class);
    g_type_class_add_private(class, sizeof(EdsacErrorNotebookPrivate));
    G_OBJECT_CLASS(class)->finalize = edsac_error_notebook_finalize;
}

// CONSTRUCT PRIVATE MEMBER DATA HERE
static void edsac_error_notebook_instance_init(EdsacErrorNotebook *self) {
    self->priv = EDSAC_ERROR_NOTEBOOK_GET_PRIVATE(self);

    self->priv->open_tabs_list = NULL; // empty slist

    Clickable *all_desc = malloc(sizeof(Clickable));
    assert(NULL != all_desc);
    all_desc->type = ALL;

    LinkyBuffer *all = (LinkyBuffer *) add_new_page_to_notebook(self, all_desc);
    assert(NULL != all);

    gtk_notebook_set_scrollable(&self->parent_instance, TRUE);
}

GType edsac_error_notebook_get_type(void) {
    static volatile gsize edsac_error_notebook_type_id_volatile = 0;
    if (g_once_init_enter(&edsac_error_notebook_type_id_volatile)) {
        static const GTypeInfo g_define_type_info = {
            sizeof(EdsacErrorNotebookClass),
            (GBaseInitFunc) NULL,
            (GBaseFinalizeFunc) NULL,
            (GClassInitFunc) edsac_error_notebook_class_init,
            (GClassFinalizeFunc) NULL,
            NULL,
            sizeof(EdsacErrorNotebook),
            0,
            (GInstanceInitFunc) edsac_error_notebook_instance_init,
            NULL
        };

        GType edsac_error_notebook_type_id;
        edsac_error_notebook_type_id = g_type_register_static(gtk_notebook_get_type(), "EdsacErrorNotebook", &g_define_type_info, 0);
        g_once_init_leave(&edsac_error_notebook_type_id_volatile, edsac_error_notebook_type_id);
    }

    return edsac_error_notebook_type_id_volatile;
}

EdsacErrorNotebook *edsac_error_notebook_new(void) {
    return edsac_error_notebook_construct(EDSAC_TYPE_ERROR_NOTEBOOK);
}
