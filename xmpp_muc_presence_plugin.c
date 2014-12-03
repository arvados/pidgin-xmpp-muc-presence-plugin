#define PURPLE_PLUGINS

#include "xmpp_muc_presence_plugin.h"

#include <gtk/gtk.h>
#include <libpurple/debug.h>
#include <libpurple/version.h>
#include <pidgin/gtkimhtml.h>
#include <pidgin/gtkplugin.h>
#include <pidgin/gtkutils.h>
#include <pidgin/pidginstock.h>

#define INVALID_TIMER_HANDLE ((guint)-1)
#define PROCESSED_KEY "xmpp_muc_presence_plugin_processed"
#define CONVERSATION_SWITCHED_KEY "xmpp_muc_presence_plugin_conv_switched"
#define PROCESSED_MARK GINT_TO_POINTER(1)
#define ICON_STOCK_NULL GINT_TO_POINTER(-1)
#define PROTOCOL_JABBER "prpl-jabber"

#define PREF_PREFIX "/plugins/gtk/" PLUGIN_ID
#define PREF_SHOW_PRESENCE PREF_PREFIX "/show_presence"

enum {
    STATE_UNKNOWN = 0,
    STATE_OFFLINE,
    STATE_AVAILABLE,
    STATE_AWAY,
    STATE_XA,
    STATE_DND,
    STATE_CHAT
};

enum {
    CONV_ICON_COLUMN,
    CONV_TEXT_COLUMN,
    CONV_EMBLEM_COLUMN,
    CONV_PROTOCOL_ICON_COLUMN,
    CONV_NUM_COLUMNS
} PidginInfopaneColumns;

static PurplePlugin* s_muc_presence = NULL;
static GHashTable* s_presence = NULL;
static GHashTable* s_original_stock = NULL;
static gboolean s_timeout_callback_update_stock_icon_all_registered = FALSE;

static gboolean is_jabber(PidginConversation* gtkconv)
{
    PurpleConversation *conv = gtkconv ? gtkconv->active_conv : NULL;
    PurpleAccount *acc = conv ? conv->account : NULL;

    if (acc &&
        strcmp(PROTOCOL_JABBER, purple_account_get_protocol_id(acc)) == 0)
    {
        return TRUE;
    }
    else
    {
        return FALSE;
    }
}

static const char* get_presence_stock_icon(int presence)
{
    switch (presence)
    {
    case STATE_AWAY:
        return PIDGIN_STOCK_STATUS_AWAY;
    case STATE_CHAT:
        return PIDGIN_STOCK_STATUS_CHAT;
    case STATE_XA:
        return PIDGIN_STOCK_STATUS_XA;
    case STATE_DND:
        return PIDGIN_STOCK_STATUS_BUSY;
    case STATE_OFFLINE:
        return PIDGIN_STOCK_STATUS_OFFLINE;
    case STATE_AVAILABLE:
    default:
        return PIDGIN_STOCK_STATUS_AVAILABLE;
    }
}

static gboolean is_processed(PidginConversation* gtkconv)
{
    PurpleConversation* conv = gtkconv ? gtkconv->active_conv : NULL;
    return purple_conversation_get_data(conv, PROCESSED_KEY) == PROCESSED_MARK;
}

static void set_processed(PidginConversation* gtkconv)
{
    PurpleConversation* conv = gtkconv ? gtkconv->active_conv : NULL;
    purple_conversation_set_data(conv, PROCESSED_KEY, PROCESSED_MARK);
}

static void memory_original_stock_icon(const char* jid, const char* stock)
{
    g_hash_table_replace(
        s_original_stock,
        (gpointer)g_strdup(jid),
        (gpointer)(stock ? stock : ICON_STOCK_NULL));
}

static gboolean lookup_original_stock_icon(const char* jid, const char** o_stock)
{
    const char* stock = g_hash_table_lookup(s_original_stock, jid);
    if (o_stock)
    {
        *o_stock = (stock && stock != ICON_STOCK_NULL) ? stock : NULL;
    }
    return stock != NULL;
}

static void
restore_chat_original_stock_icon(PidginConversation* gtkconv)
{
    PurpleConversation* conv = gtkconv ? gtkconv->active_conv : NULL;
    PurpleConvChat* chat = conv ? PURPLE_CONV_CHAT(conv) : NULL;
    PidginChatPane* gtkchat = gtkconv ? gtkconv->u.chat : NULL;
    GtkTreeModel* tm = gtkchat ? gtk_tree_view_get_model(GTK_TREE_VIEW(gtkchat->list)) : NULL;
    GtkListStore* ls = tm ? GTK_LIST_STORE(tm) : NULL;

    if (!is_jabber(gtkconv) || !is_processed(gtkconv))
    {
        return;
    }

    if (conv && chat && tm && ls)
    {
        GtkTreeIter iter;

        if (gtk_tree_model_get_iter_first(tm, &iter))
        {
            do
            {
                gchar* alias = NULL;
                gchar* name = NULL;
                const char* currStock = NULL;
                const char* origStock = NULL;

                gtk_tree_model_get(
                  tm,
                  &iter,
                  CHAT_USERS_ALIAS_COLUMN, &alias,
                  CHAT_USERS_NAME_COLUMN, &name,
                  CHAT_USERS_ICON_STOCK_COLUMN, &currStock,
                  -1);

                {
                    char* jid = g_strdup_printf("%s/%s", conv->name, alias ? alias : name);

                    if (lookup_original_stock_icon(jid, &origStock))
                    {
                        if (currStock != origStock)
                        {
                            gtk_list_store_set(
                                ls,
                                &iter,
                                CHAT_USERS_ICON_STOCK_COLUMN, origStock,
                                -1);
                        }
                    }

                    g_free(jid);
                }
            }
            while (gtk_tree_model_iter_next(tm, &iter));
        }
    }
}

static void
set_chat_presence_stock_icon(PidginConversation* gtkconv)
{
    PurpleConversation* conv = gtkconv ? gtkconv->active_conv : NULL;
    PurpleConvChat* chat = conv ? PURPLE_CONV_CHAT(conv) : NULL;
    PidginChatPane* gtkchat = gtkconv ? gtkconv->u.chat : NULL;
    GtkTreeModel* tm = gtkchat ? gtk_tree_view_get_model(GTK_TREE_VIEW(gtkchat->list)) : NULL;
    GtkListStore* ls = tm ? GTK_LIST_STORE(tm) : NULL;

    if (!is_jabber(gtkconv))
    {
        return;
    }

    if (gtkconv && conv && chat && tm && ls)
    {
        GtkTreeIter iter;
        gboolean memory = FALSE;

        if (!is_processed(gtkconv))
        {
            memory = TRUE; 
            set_processed(gtkconv);
        }

        if (gtk_tree_model_get_iter_first(tm, &iter))
        {
            do
            {
                gchar* alias = NULL;
                gchar* name = NULL;
                int state = STATE_AVAILABLE;
                const char* currStock = NULL;
                const char* stock = NULL;

                gtk_tree_model_get(
                  tm,
                  &iter,
                  CHAT_USERS_ALIAS_COLUMN, &alias,
                  CHAT_USERS_NAME_COLUMN, &name,
                  CHAT_USERS_ICON_STOCK_COLUMN, &currStock,
                  -1);

                {
                    char* jid = g_strdup_printf("%s/%s", conv->name, alias ? alias : name);

                    state = (int)g_hash_table_lookup(s_presence, jid);
                    if (memory || !lookup_original_stock_icon(jid, NULL))
                    {
                        memory_original_stock_icon(jid, currStock);
                    }

                    g_free(jid);
                }

                if (!purple_conv_chat_is_user_ignored(chat, name))
                {
                    if (state != STATE_UNKNOWN)
                    {
                        stock = get_presence_stock_icon(state);

                        if (!currStock || strcmp(currStock, stock) != 0)
                        {
                            gtk_list_store_set(ls, &iter, CHAT_USERS_ICON_STOCK_COLUMN, stock, -1);
                        }
                    }
                }
            }
            while (gtk_tree_model_iter_next(tm, &iter));
        }
    }
}

static void
restore_im_original_stock_icon(PidginConversation* gtkconv)
{
    PurpleConversation* conv = gtkconv ? gtkconv->active_conv : NULL;

    if (!is_jabber(gtkconv))
    {
        return;
    }

    if (conv)
    {
        purple_conversation_update(conv, PURPLE_CONV_UPDATE_AWAY);
    }
}

static void
set_im_presence_stock_icon(PidginConversation* gtkconv)
{
    PurpleConversation* conv = gtkconv ? gtkconv->active_conv : NULL;

    if (!is_jabber(gtkconv))
    {
        return;
    }

    if (gtkconv && conv)
    {
        int state = STATE_AVAILABLE;

        {
            char* jid = conv->name;
            state = (int)g_hash_table_lookup(s_presence, jid);
        }

        if (state != STATE_UNKNOWN)
        {
            const char* stock = stock = get_presence_stock_icon(state);

            // tab icon.
            g_object_set(G_OBJECT(gtkconv->icon), "stock", stock, NULL);
            // menu icon.
            g_object_set(G_OBJECT(gtkconv->menu_icon), "stock", stock, NULL);
            // icon at top of messages.
            gtk_list_store_set(
                gtkconv->infopane_model,
                &(gtkconv->infopane_iter),
                CONV_ICON_COLUMN, stock,
                -1);

            gtk_widget_queue_draw(gtkconv->infopane);
        }
    }
}

static void
update_stock_icon(PidginConversation* gtkconv)
{
    PurpleConversation* conv = gtkconv ? gtkconv->active_conv : NULL;

    switch (purple_conversation_get_type(conv))
    {
    case PURPLE_CONV_TYPE_CHAT:
        if (purple_prefs_get_bool(PREF_SHOW_PRESENCE))
        {
            set_chat_presence_stock_icon(gtkconv);
        }
        else
        {
            restore_chat_original_stock_icon(gtkconv);
        }
        break;

    case PURPLE_CONV_TYPE_IM:
        if (purple_prefs_get_bool(PREF_SHOW_PRESENCE))
        {
            set_im_presence_stock_icon(gtkconv);
        }
        else
        {
            restore_im_original_stock_icon(gtkconv);
        }
        break;

    default:
        break;
    }
}

static void
update_stock_icon_all()
{
    GList* list;

    for (list = pidgin_conv_windows_get_list();
         list;
         list = list->next)
    {
        PidginWindow* window = list->data;
        GList* gtkconvs = window ? pidgin_conv_window_get_gtkconvs(window) : NULL;

        for (; gtkconvs; gtkconvs = gtkconvs->next)
        {
            PidginConversation* gtkconv = gtkconvs->data;
            PurpleConversation* conv = gtkconv ? gtkconv->active_conv : NULL;

            if (gtkconv == pidgin_conv_window_get_active_gtkconv(window) ||
                purple_conversation_get_type(conv) == PURPLE_CONV_TYPE_IM)
            {
                update_stock_icon(gtkconv);
            }
        }
    }
}

static gboolean
timeout_callback_conversation_switched(PurpleConversation* conv)
{
    purple_debug_info(PLUGIN_ID, "timeout_callback_conversation_switched\n");

    purple_conversation_set_data(conv, CONVERSATION_SWITCHED_KEY, (gpointer)FALSE);

    update_stock_icon(PIDGIN_CONVERSATION(conv));

    return FALSE;
}

static void
handle_conversation_switched(PurpleConversation* conv)
{
    purple_debug_info(PLUGIN_ID, "handle_conversation_switched\n");

    if (!pidgin_conv_is_hidden(PIDGIN_CONVERSATION(conv)) &&
        !purple_conversation_get_data(conv, CONVERSATION_SWITCHED_KEY))
    {
        purple_conversation_set_data(conv, CONVERSATION_SWITCHED_KEY, (gpointer)TRUE);

        // invoke update_stock_icon() after handling of current signal is finished.
        purple_timeout_add_seconds(
            0 /* interval */,
            (GSourceFunc)timeout_callback_conversation_switched,
            conv);
    }
}

static void
handle_toggle_presence_icon(gpointer data)
{
    purple_debug_info(PLUGIN_ID, "handle_toggle_presence_icon\n");

    purple_prefs_set_bool(
        PREF_SHOW_PRESENCE,
        !purple_prefs_get_bool(PREF_SHOW_PRESENCE));

    update_stock_icon_all();
}

static void
handle_conversation_extended_menu(PurpleConversation* conv, GList** list)
{
    purple_debug_info(PLUGIN_ID, "handle_conversation_extended_menu\n");

    if (purple_conversation_get_type(conv) == PURPLE_CONV_TYPE_CHAT ||
        purple_conversation_get_type(conv) == PURPLE_CONV_TYPE_IM)
    {
        PurpleMenuAction* action = purple_menu_action_new(
            "Toggle Presence Icon",
            PURPLE_CALLBACK(handle_toggle_presence_icon),
            NULL /* data */,
            NULL /* children */);
        *list = g_list_append(*list, action);
    }
}

static gboolean
timeout_callback_update_stock_icon_all()
{
    purple_debug_info(PLUGIN_ID, "timeout_callback_update_stock_icon_all\n");

    s_timeout_callback_update_stock_icon_all_registered = FALSE;

    update_stock_icon_all();
    return FALSE;
}

static gboolean
handle_jabber_receiving_presence(
    PurpleConnection* pc,
    const char* type,
    const char* from,
    xmlnode* presence)
{
    xmlnode* show = NULL;
    char* showText = NULL;
    int state = STATE_UNKNOWN;

    purple_debug_info(PLUGIN_ID, "handle_jabber_receiving_presence %s %s\n", type, from);

    show = xmlnode_get_child(presence, "show");
    showText = show ? xmlnode_get_data(show) : NULL;

    purple_debug_info(PLUGIN_ID, "  show %s\n", showText);

    if (type)
    {
        if (strcmp(type, "unavailable") == 0)
        {
            state = STATE_OFFLINE;
        }
    }

    if (state == STATE_UNKNOWN)
    {
        if (showText)
        {
            if (strcmp(showText, "away") == 0)
            {
                state = STATE_AWAY;
            }
            else if (strcmp(showText, "dnd") == 0)
            {
                state = STATE_DND;
            }
            else if (strcmp(showText, "xa") == 0)
            {
                state = STATE_XA;
            }
            else if (strcmp(showText, "chat") == 0)
            {
                state = STATE_CHAT;
            }
            else
            {
                state = STATE_AVAILABLE;
            }
        }
        else
        {
            state = STATE_AVAILABLE;
        }
    }

    g_hash_table_replace(s_presence, (gpointer)g_strdup(from), GINT_TO_POINTER(state));

    if (purple_prefs_get_bool(PREF_SHOW_PRESENCE) &&
        !s_timeout_callback_update_stock_icon_all_registered)
    {
        s_timeout_callback_update_stock_icon_all_registered = TRUE;

        // invoke update_stock_icon_all() after handling of current signal is finished.
        purple_timeout_add_seconds(
            0 /* interval */,
            (GSourceFunc)timeout_callback_update_stock_icon_all,
            NULL /* data */);
    }

    // don't stop signal processing
    return FALSE;
}

static gboolean
plugin_load(PurplePlugin *plugin)
{
    PurplePlugin* jabber = NULL;

    s_presence = g_hash_table_new_full(
        g_str_hash,
        g_str_equal,
        (GDestroyNotify)g_free,
        (GDestroyNotify)NULL);

    s_original_stock = g_hash_table_new(
        g_str_hash,
        g_str_equal);

    s_muc_presence = plugin;

    purple_signal_connect(
        pidgin_conversations_get_handle(),
        "conversation-switched",
        plugin, PURPLE_CALLBACK(handle_conversation_switched), NULL);

    purple_signal_connect(
        purple_conversations_get_handle(),
        "conversation-extended-menu",
        plugin, PURPLE_CALLBACK(handle_conversation_extended_menu), NULL);

    if ((jabber = purple_find_prpl(PROTOCOL_JABBER)) != NULL)
    {
        purple_signal_connect(
            jabber,
            "jabber-receiving-presence",
            plugin, PURPLE_CALLBACK(handle_jabber_receiving_presence), NULL);
    }

    return TRUE;
}

static gboolean
plugin_unload(PurplePlugin *plugin)
{
    g_hash_table_destroy(s_presence);
    g_hash_table_destroy(s_original_stock);
    return TRUE;
}

static PurplePluginInfo info = {
    PURPLE_PLUGIN_MAGIC,
    PURPLE_MAJOR_VERSION,
    PURPLE_MINOR_VERSION,
    PURPLE_PLUGIN_STANDARD,
    PIDGIN_PLUGIN_TYPE,
    0,
    NULL,
    PURPLE_PRIORITY_DEFAULT,

    PLUGIN_ID,
    "XMPP MUC Presence plugin",
    "1.3",

    "XMPP MUC Presence plugin",
    "Show status icon in chatroom",
    "Takashi Matsuda <matsu@users.sf.net>",
    "https://github.com/tmatz/pidgin-xmpp-muc-presence-plugin",

    plugin_load,
    plugin_unload,
    NULL, /* destroy */

    NULL, /* ui_info */
    NULL, /* extra_info */
    NULL, /* prefs_into */
    NULL, /* plugin_actions */

    /* padding */
    NULL,
    NULL,
    NULL,
    NULL
};

static void
init_plugin(PurplePlugin *plugin)
{
    purple_prefs_add_none(PREF_PREFIX);
    purple_prefs_add_bool(PREF_SHOW_PRESENCE, TRUE);
}

PURPLE_INIT_PLUGIN(xmpp_muc_presence_plugin, init_plugin, info)
