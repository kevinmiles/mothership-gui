/*
 * Copyright 2017
 * GPL3 Licensed
 * ui.c
 * UI stuff other than EdsacErrorNotebook
 */

// includes
#include "config.h"
#include "ui.h"
#include <assert.h>
#include <gtk/gtk.h>
#include "EdsacErrorNotebook.h"
#include "sql.h"
#include <edsac_server.h>
#include <edsac_timer.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>
#include "node_setup.h"

extern const char * g_prefix_path; // main.c

// declarations
static void activate(GtkApplication *app, gpointer data);
static void shutdown_handler(__attribute__((unused)) GApplication *app, gpointer user_data);

static EdsacErrorNotebook *notebook = NULL;
static GtkStatusbar *bar = NULL;
static GtkWindow *main_window = NULL;
static GMenu *model = NULL;

// functions

int start_ui(int *argc, char ***argv, gpointer timer_id) {
    assert(NULL != argc);
    assert(NULL != argv);
    assert(NULL != timer_id);

    gtk_init(argc, argv);

    GtkApplication *app = gtk_application_new("edsac.motherhip.gui", G_APPLICATION_FLAGS_NONE);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    g_signal_connect(app, "shutdown", G_CALLBACK(shutdown_handler), timer_id);
    
    return g_application_run(G_APPLICATION(app), *argc, *argv);
}

static void update_bar(void) {
    const int num_errors = edsac_error_notebook_get_error_count(notebook);

    GString *msg = g_string_new(NULL);
    assert(NULL != msg);

    if (num_errors > 1) {
        g_string_sprintf(msg, "Showing %i errors", num_errors);
    } else if (1 == num_errors) {
        g_string_sprintf(msg, "Showing 1 error");
    } else if (0 == num_errors) {
        g_string_sprintf(msg, "No errors in this filter");
    } else { // negative
        g_string_sprintf(msg, "Failure to count errors (something is probably very wrong)");
    }

    if (!get_show_disabled()) {
        g_string_append(msg, " (disabled items hidden and not counted)");
    }

    gtk_statusbar_pop(bar, 0);
    gtk_statusbar_push(bar, 0, msg->str);
    g_string_free(msg, TRUE);
}

// called when gtk gets around to updating the gui
void gui_update(gpointer g_idle_id) {
    if (NULL != g_idle_id) {
        assert(TRUE == g_idle_remove_by_data(g_idle_id));
    }
    edsac_error_notebook_update(notebook);
    update_bar();
}

// handles the quit action
static void quit_activate(void) {
    if (NULL != main_window) {
        gtk_window_close(main_window);
    }
}

static GMenu *generate_nodes_menu(void) {
    GMenu *nodes = g_menu_new();
    assert(NULL != nodes);

    GList *racks = list_racks();
    while (NULL != racks) {
        const uintptr_t rack_no = (uintptr_t) racks->data;
        char rack_label[10];
        snprintf(rack_label, 10, "Rack %li", rack_no);

        GMenu *rack = g_menu_new();

        GList *chassis = list_chassis_by_rack(rack_no);
        while (NULL != chassis) {
            const uintptr_t chassis_no = (uintptr_t) chassis->data;
            char chassis_label[15];
            snprintf(chassis_label, 15, "Chassis %li", chassis_no);

            GMenu *node = g_menu_new();
            assert(NULL != node);
            
            GMenuItem *show = g_menu_item_new("Show", "app.node_show");
            assert(NULL != show);
            g_menu_item_set_action_and_target_value(show, "app.node_show", g_variant_new("(tt)", rack_no, chassis_no));
            g_menu_append_item(node, show);

            GMenuItem *disable = g_menu_item_new("Toggle Disabled", "app.node_toggle_disabled");
            assert(NULL != disable);
            g_menu_item_set_action_and_target_value(disable, "app.node_toggle_disabled", g_variant_new("(tt)", rack_no, chassis_no));
            g_menu_append_item(node, disable);

            GMenuItem *delete = g_menu_item_new("Delete", "app.node_delete");
            assert(NULL != delete);
            g_menu_item_set_action_and_target_value(delete, "app.node_delete", g_variant_new("(tt)", rack_no, chassis_no));
            g_menu_append_item(node, delete);

            g_menu_freeze(node);
            g_menu_append_submenu(rack, chassis_label, G_MENU_MODEL(node));

            chassis = chassis->next;
        }

        g_menu_freeze(rack);
        g_menu_append_submenu(nodes, rack_label, G_MENU_MODEL(rack));
        racks = racks->next;
    } 

    g_menu_freeze(nodes);

    return nodes;
}

static void update_nodes_menu(void) {
    g_menu_remove(model, 2);
    g_menu_append_submenu(model, "Nodes", G_MENU_MODEL(generate_nodes_menu()));
}

static void choose_config_file_callback(__attribute__((unused)) GtkButton *unused, gpointer user_data) {
    assert(NULL != user_data);
    GtkTextBuffer *buffer = GTK_TEXT_BUFFER(user_data);

    GtkWidget *dialog = gtk_file_chooser_dialog_new("Choose Configuration Archive", main_window, 
        GTK_FILE_CHOOSER_ACTION_OPEN, "Cancel", GTK_RESPONSE_CANCEL,
        "Open", GTK_RESPONSE_ACCEPT, NULL);

    // set directory to start from as the configs directory 
    GString *configs_path = g_string_new(g_prefix_path);
    assert(NULL != configs_path);
    g_string_append(configs_path, "/configs");
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), configs_path->str);

    gint res = gtk_dialog_run(GTK_DIALOG(dialog));
    if (GTK_RESPONSE_ACCEPT == res) {
        GtkFileChooser *chooser = GTK_FILE_CHOOSER(dialog);
        assert(NULL != chooser);
        gtk_text_buffer_set_text(buffer, gtk_file_chooser_get_filename(chooser), -1);
    }

    gtk_widget_destroy(dialog);
}

static char *get_all_text(GtkTextBuffer *buffer) {
    assert(NULL != buffer);

    GtkTextIter start;
    gtk_text_buffer_get_start_iter(buffer, &start);
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buffer, &end);

    return gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
}

static void set_error_text(GtkTextBuffer *buffer) {
    assert(NULL != buffer);

    GtkTextIter start;
    gtk_text_buffer_get_start_iter(buffer, &start);
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buffer, &end);

    GtkTextTag *error_text = gtk_text_buffer_create_tag(buffer, NULL, 
        "underline", PANGO_UNDERLINE_SINGLE, "underline-set", TRUE,
        "foreground", "red", NULL);
    assert(NULL != error_text);

    gtk_text_buffer_apply_tag(buffer, error_text, &start, &end);
}

static bool is_uint(GtkTextBuffer *buffer) {
    assert(NULL != buffer);

    char *text = get_all_text(buffer);

    bool ret = true;
    for (char *text_iter = text; *text_iter != '\0'; text_iter++) {
        ret &= g_ascii_isdigit(*text_iter); // signed would start with '-'
    }

    g_free(text);

    if (!ret) {
        set_error_text(buffer);
    }

    return ret;
}

static GtkTextBuffer *extract_buffer(GtkGrid *grid, gint left, gint top) {
    assert(NULL != grid);

    GtkBin *frame = GTK_BIN(gtk_grid_get_child_at(grid, left, top));
    GtkTextView *view = GTK_TEXT_VIEW(gtk_bin_get_child(frame));

    return gtk_text_view_get_buffer(view);
}

static void ok_callback(__attribute__((unused)) GtkButton *unused, gpointer user_data) {
    char *config_path = NULL;
    assert(NULL != user_data);
    GtkWindow *add_node_window = GTK_WINDOW(user_data);
    GtkGrid *grid = GTK_GRID(gtk_bin_get_child(GTK_BIN(add_node_window)));

    GtkTextBuffer *rack_no_buffer = extract_buffer(grid, 1, 0);
    GtkTextBuffer *chassis_no_buffer = extract_buffer(grid, 1, 1);
    GtkTextBuffer *mac_address_buffer = extract_buffer(grid, 1, 2);
    GtkTextBuffer *config_file_buffer = extract_buffer(grid, 1, 3);
    GtkToggleButton *toggle_button = GTK_TOGGLE_BUTTON(gtk_grid_get_child_at(grid, 0, 4));

    bool valid = is_uint(rack_no_buffer);
    valid &= is_uint(chassis_no_buffer);

    char *mac_addr = get_all_text(mac_address_buffer);
    assert(NULL != mac_addr);

    // get rack_no and chassis_no as numbers (safe because we already checked they are numbers)
    char *rack_no_str = get_all_text(rack_no_buffer);
    assert(NULL!= rack_no_str);
    char *chassis_no_str = get_all_text(chassis_no_buffer);
    assert(NULL != chassis_no_str);

    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wsign-conversion"
    const unsigned int rack_no = atoi(rack_no_str);
    const unsigned int chassis_no = atoi(chassis_no_str);
    #pragma GCC diagnostic pop

    // check that the node doesn't already exist
    if (node_exists(rack_no, chassis_no)) {
        valid = false;
        set_error_text(rack_no_buffer);
        set_error_text(chassis_no_buffer);

        // dialog box to say so
        GtkWidget *dialog = gtk_message_dialog_new(add_node_window, GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_ERROR,
                GTK_BUTTONS_CLOSE, "Node at rack %i, chassis %i already in database!", rack_no, chassis_no);
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
    }

    if (valid) { // if everything so far was valid
        // do we set up the node?
        if (gtk_toggle_button_get_active(toggle_button)) {
            // we should set up node

            // error check stuff we didn't care about before
            if (!check_mac_address(mac_addr)) {
                set_error_text(mac_address_buffer);
                valid = false;
            }

            config_path = get_all_text(config_file_buffer);
            assert(NULL != config_path);
            if (F_OK != access(config_path, R_OK)) {
                set_error_text(config_file_buffer);
                valid = false;
            }
            
            if (!valid) {
                goto clean_up;
            }

            // actually set up the node
            if (!setup_node_network(rack_no, chassis_no, mac_addr)) {
                // complain
                GtkWidget *bad_setup_dialog = gtk_message_dialog_new(add_node_window, GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_ERROR,
                    GTK_BUTTONS_CLOSE, "Failed to set up node!");
                gtk_dialog_run(GTK_DIALOG(bad_setup_dialog));
                gtk_widget_destroy(bad_setup_dialog);
            } else {
                // else it worked
                // ask the user to boot the node
                GtkWidget *delay_dialog = gtk_message_dialog_new(add_node_window, GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_INFO,
                    GTK_BUTTONS_OK, "Please press OK once the node is (re)booted");
                gtk_dialog_run(GTK_DIALOG(delay_dialog));
                gtk_widget_destroy(delay_dialog);

                // ssh stage of node setup
                if (!setup_node_ssh(rack_no, chassis_no, config_path)) {
                    // complain
                    GtkWidget *bad_copy_dialog = gtk_message_dialog_new(add_node_window, GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_ERROR,
                        GTK_BUTTONS_CLOSE, "Failed to setup node over ssh!");
                    gtk_dialog_run(GTK_DIALOG(bad_copy_dialog));
                    gtk_widget_destroy(bad_copy_dialog);
                } 
            }
        }

        // add the node to the database
        add_node(rack_no, chassis_no, true);
        update_nodes_menu();
 
        g_object_unref(G_OBJECT(config_file_buffer)); // ref'ed in add_node_activate
        gtk_window_close(add_node_window);
    }

    // clean up
    clean_up:
    g_free(rack_no_str);
    g_free(chassis_no_str);
    g_free(config_path);
    g_free(mac_addr);
}

static void clear_text_tags(GtkTextBuffer *buffer) {
    assert(NULL != buffer);

    GtkTextIter start;
    gtk_text_buffer_get_start_iter(buffer, &start);
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buffer, &end);

    gtk_text_buffer_remove_all_tags(buffer, &start, &end);
}

static GtkTextView *new_text_view(GtkGrid *grid, gint left, gint top) {
    assert(NULL != grid);

    GtkWidget *text = gtk_text_view_new();
    assert(NULL != text);
    gtk_text_view_set_accepts_tab(GTK_TEXT_VIEW(text), FALSE);
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text));
    g_object_ref(G_OBJECT(buffer));
    g_signal_connect_swapped(G_OBJECT(text), "grab-focus", G_CALLBACK(clear_text_tags), buffer);
    GtkWidget *frame = gtk_frame_new(NULL);
    assert(NULL != frame);
    gtk_container_add(GTK_CONTAINER(frame), text);
    gtk_grid_attach(grid, frame, left, top, 1, 1);

    gtk_text_view_set_input_hints(GTK_TEXT_VIEW(text), GTK_INPUT_HINT_NO_EMOJI | GTK_INPUT_HINT_NO_SPELLCHECK);

    return GTK_TEXT_VIEW(text);
}

// handles the add_node action
static void add_node_activate(void) {
    GtkWindow *add_node_window = GTK_WINDOW(gtk_window_new(GTK_WINDOW_TOPLEVEL));
    assert(NULL != add_node_window);
//    gtk_window_set_decorated(add_node_window, FALSE);
    gtk_window_set_modal(add_node_window, TRUE);
    gtk_window_set_transient_for(add_node_window, main_window);
    gtk_window_set_title(add_node_window, "Add Node");
    gtk_container_set_border_width(GTK_CONTAINER(add_node_window), 10);

    GtkGrid *grid = GTK_GRID(gtk_grid_new());
    gtk_grid_set_column_spacing(grid, 5);
    gtk_grid_set_row_spacing(grid, 5);
    gtk_grid_set_row_homogeneous(grid, TRUE);
    gtk_grid_set_column_homogeneous(grid, TRUE);
    assert(NULL != grid);

    GtkWidget *rack_no_label = gtk_label_new("Rack Number");
    assert(NULL != rack_no_label);
    gtk_grid_attach(grid, rack_no_label, 0, 0, 1, 1);

    GtkTextView *rack_no_text = new_text_view(grid, 1, 0);
    gtk_text_view_set_input_purpose(rack_no_text, GTK_INPUT_PURPOSE_DIGITS);

    GtkWidget *chassis_no_label = gtk_label_new("Chassis Number");
    assert(NULL != chassis_no_label);
    gtk_grid_attach(grid, chassis_no_label, 0, 1, 1, 1);

    GtkTextView *chassis_no_text = new_text_view(grid, 1, 1);
    gtk_text_view_set_input_purpose(chassis_no_text, GTK_INPUT_PURPOSE_DIGITS);

    GtkWidget *mac_address_label = gtk_label_new("Mac Address");
    assert(NULL != mac_address_label);
    gtk_grid_attach(grid, mac_address_label, 0, 2, 1, 1);

    GtkTextView *mac_address_text = new_text_view(grid, 1, 2);
    gtk_text_view_set_input_hints(mac_address_text, GTK_INPUT_HINT_LOWERCASE | GTK_INPUT_HINT_EMOJI | GTK_INPUT_HINT_NO_SPELLCHECK);

    GtkTextView *config_path_text = new_text_view(grid, 1, 3);

    GtkWidget *config_path_button = gtk_button_new_with_label("Choose config archive");
    assert(NULL != config_path_button);
    gtk_grid_attach(grid, config_path_button, 0, 3, 1, 1);
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(config_path_text);
    g_object_ref(G_OBJECT(buffer));
    g_signal_connect(config_path_button, "pressed", G_CALLBACK(choose_config_file_callback), buffer);

    GtkWidget *setup_toggle = gtk_check_button_new_with_label("Set up node");
    assert(NULL != setup_toggle);
    gtk_grid_attach(grid, setup_toggle, 0, 4, 1, 1);

    GtkWidget *ok_button = gtk_button_new_with_label("Ok");
    assert(NULL != ok_button);
    gtk_grid_attach(grid, ok_button, 1, 4, 1, 1);
    g_signal_connect(ok_button, "pressed", G_CALLBACK(ok_callback), add_node_window);

    gtk_container_add(GTK_CONTAINER(add_node_window), GTK_WIDGET(grid));
    gtk_widget_show_all(GTK_WIDGET(add_node_window));
}

static void hide_disabled_change_state(GSimpleAction *simple) {
    assert(NULL != simple);
    gboolean hide_disabled = g_variant_get_boolean(g_action_get_state(G_ACTION(simple)));

    // toggle
    if (hide_disabled) {
        g_simple_action_set_state(simple, g_variant_new_boolean(FALSE));
        //puts("Now Showing disabled items");
        set_show_disabled(true);
        gui_update(NULL);
   } else {
        g_simple_action_set_state(simple, g_variant_new_boolean(TRUE));
        //puts("Now hiding disabled items");
        set_show_disabled(false);
        gui_update(NULL);
   }
}

static void node_toggle_disabled_activate(__attribute__((unused)) GSimpleAction *simple, GVariant *parameter) {
    assert(NULL != parameter);

    GVariant *rack_variant = g_variant_get_child_value(parameter, 0);
    GVariant *chassis_variant = g_variant_get_child_value(parameter, 1);

    uint64_t rack_no = g_variant_get_uint64(rack_variant);
    uint64_t chassis_no = g_variant_get_uint64(chassis_variant);

    g_variant_unref(rack_variant);
    g_variant_unref(chassis_variant);

    assert(true == node_toggle_disabled(rack_no, chassis_no));
    
    printf("Node %li %li toggled\n", rack_no, chassis_no);

    gui_update(NULL);
}

static void node_delete_activate(__attribute__((unused)) GSimpleAction *simple, GVariant *parameter) {
    assert(NULL != parameter);

    GVariant *rack_variant = g_variant_get_child_value(parameter, 0);
    GVariant *chassis_variant = g_variant_get_child_value(parameter, 1);

    uint64_t rack_no = g_variant_get_uint64(rack_variant);
    uint64_t chassis_no = g_variant_get_uint64(chassis_variant);

    g_variant_unref(rack_variant);
    g_variant_unref(chassis_variant);

    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wconversion"
    assert(true == remove_node(rack_no, chassis_no));
    #pragma GCC diagnostic pop

    edsac_error_notebook_close_node(notebook, (unsigned int) rack_no, (unsigned int) chassis_no);

    // clean up node network configuration
    node_cleanup_network((unsigned int) rack_no, (unsigned int) chassis_no);

    printf("Node %li %li removed\n", rack_no, chassis_no);

    update_nodes_menu();
    gui_update(NULL);
}

static void node_show_activate(__attribute__((unused)) GSimpleAction *simple, GVariant *parameter) {
    assert(NULL != parameter);

    GVariant *rack_variant = g_variant_get_child_value(parameter, 0);
    GVariant *chassis_variant = g_variant_get_child_value(parameter, 1);

    uint64_t rack_no = g_variant_get_uint64(rack_variant);
    uint64_t chassis_no = g_variant_get_uint64(chassis_variant);

    g_variant_unref(rack_variant);
    g_variant_unref(chassis_variant);

    Clickable search;
    search.type = CHASSIS;
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wconversion"
    search.rack_num = rack_no;
    search.chassis_num = chassis_no;
    #pragma GCC diagnostic pop

    edsac_error_notebook_show_page(notebook, &search);
}   

static void convert_to_nodeidentifiers(gpointer data, gpointer user_data) {
    assert(NULL != data);
    assert(NULL != user_data);

    struct sockaddr_in *addr_struct = data;
    GSList **list = user_data;

    NodeIdentifier *list_data = parse_ip_address(&addr_struct->sin_addr);
    assert(NULL != list_data);

    *list = g_slist_prepend(*list, list_data);
}

static gint compare_nodeids(gconstpointer a, gconstpointer b) {
    assert(NULL != a);
    assert(NULL != b);

    const NodeIdentifier *A = a;
    const NodeIdentifier *B = b;

    if ((A->chassis_no == B->chassis_no) && (A->rack_no == B->rack_no)) {
        return 0;
    } else {
        return 1;
    }
}

static void complain_missing_nodes(gpointer data, gpointer user_data) {
    assert(NULL != data);

    NodeIdentifier *db_id = data;
    GSList *server_nodes = user_data;

    GSList *res = g_slist_find_custom(server_nodes, data, compare_nodeids);
    if (NULL == res) {
        // not found so make an error about it
        assert(true == add_error_decoded(db_id->rack_no, db_id->chassis_no, -1, time(NULL), "Node not connected"));
    }
}

static void check_connected_activate(void) {
    GSList *ip_addr_structs = get_connected_list();

    // convert to NodeIdentifiers
    GSList *ip_addrs_server = NULL;
    g_slist_foreach(ip_addr_structs, convert_to_nodeidentifiers, &ip_addrs_server);
    g_slist_free_full(ip_addr_structs, g_free);

    GSList *ip_addrs_db = list_nodes();    
    g_slist_foreach(ip_addrs_db, complain_missing_nodes, ip_addrs_server);

    g_slist_free_full(ip_addrs_server, g_free);
    g_slist_free_full(ip_addrs_db, g_free);

    gui_update(NULL);
}

typedef void (*action_handler_t)(GSimpleAction *simple, GVariant *parameter, gpointer user_data);

// activate handler for the application
static void activate(GtkApplication *app, __attribute__((unused)) gpointer data) {
    main_window = GTK_WINDOW(gtk_application_window_new(app));
    gtk_window_set_title(main_window, "EDSAC Status Monitor");
    //gtk_window_maximize(GTK_WINDOW(WINDOW));

    // set minimum window size
    GdkGeometry geometry;
    geometry.min_height = 400;
    geometry.min_width = 600;
    gtk_window_set_geometry_hints(main_window, NULL, &geometry, GDK_HINT_MIN_SIZE);

    // GTKBox to hold stuff
    GtkBox *box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0 /*arbitrary: a guess*/));

    // Menu bar actions
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wmissing-field-initializers"
    static const GActionEntry actions[] = {
        {"add_node", (action_handler_t) add_node_activate},
        {"quit", (action_handler_t) quit_activate},
        {"check_connected", (action_handler_t) check_connected_activate},
        {"hide_disabled", NULL, "b", "true", (action_handler_t) hide_disabled_change_state},
        {"node_show", (action_handler_t) node_show_activate, "(tt)"},
        {"node_toggle_disabled", (action_handler_t) node_toggle_disabled_activate, "(tt)"},
        {"node_delete", (action_handler_t) node_delete_activate, "(tt)"}
    };
    #pragma GCC diagnostic pop
    g_action_map_add_action_entries(G_ACTION_MAP(app), actions, G_N_ELEMENTS(actions), NULL);

    // File menu model 
    GMenu *file = g_menu_new();
    assert(NULL != file);
    g_menu_append(file, "Add Node", "app.add_node");
    const char *add_accels[] = {"<Control>N", NULL};
    gtk_application_set_accels_for_action(app, "app.add_node", add_accels);
    g_menu_append(file, "Check Connections", "app.check_connected");
    g_menu_append(file, "Quit", "app.quit");
    const char *quit_accels[] = {"<Control>Q", NULL};
    gtk_application_set_accels_for_action(app, "app.quit", quit_accels);
    g_menu_freeze(file);

    // View menu model
    GMenu *view = g_menu_new();
    assert(NULL != view);
    GMenuItem *hide_disabled = g_menu_item_new("Hide Disabled", "app.hide_disabled");
    assert(NULL != hide_disabled);
    g_menu_item_set_action_and_target_value(hide_disabled, "app.hide_disabled", g_variant_new_boolean(TRUE));
    g_menu_append_item(view, hide_disabled);
    g_menu_freeze(view);

    // Nodes menu model
    GMenu *nodes = generate_nodes_menu();
    
    // Menu bar model
    model = g_menu_new();
    assert(NULL != model);
    g_menu_append_submenu(model, "File", G_MENU_MODEL(file));
    g_menu_append_submenu(model, "View", G_MENU_MODEL(view));
    g_menu_append_submenu(model, "Nodes", G_MENU_MODEL(nodes));
    g_menu_freeze(model);

    // Menu bar widget
    GtkWidget *menu = gtk_menu_bar_new_from_model(G_MENU_MODEL(model));
    assert(NULL != menu);
    gtk_box_pack_start(box, menu, FALSE, FALSE, 0);

    // make notebook
    notebook = edsac_error_notebook_new();
    g_signal_connect_after(G_OBJECT(notebook), "switch-page", G_CALLBACK(update_bar), NULL);
    gtk_box_pack_start(box, GTK_WIDGET(notebook), TRUE, TRUE, 0);

    // make status bar
    bar = GTK_STATUSBAR(gtk_statusbar_new());
    assert(NULL != bar);
    update_bar();
    gtk_box_pack_start(box, GTK_WIDGET(bar), FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(main_window), GTK_WIDGET(box));
    gtk_widget_show_all(GTK_WIDGET(main_window));
}

// handler called just before we terminate
static void shutdown_handler(__attribute__((unused)) GApplication *app, gpointer user_data) {
    if (NULL != user_data) {
        timer_t *timer_id = user_data;
        stop_timer(*timer_id);
    }

    stop_server();
    close_database();
}