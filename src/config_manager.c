#include <yaml.h>
#include "config_manager.h"

typedef struct _IpcamConfigManagerPrivate
{
    gchar key[PATH_MAX];
    GHashTable *conf_hash;
    GHashTable *collection;
} IpcamConfigManagerPrivate;

G_DEFINE_TYPE_WITH_PRIVATE(IpcamConfigManager, ipcam_config_manager, G_TYPE_OBJECT);

static void process_layer(yaml_parser_t *parser, GNode *data);
static gboolean to_hash(GNode *n, gpointer data);
static gboolean destroy(GNode *n, gpointer data);

static void ipcam_config_manager_dispose(GObject *self)
{
    static gboolean first_run = TRUE;
    if (first_run)
    {
        first_run = FALSE;
        G_OBJECT_CLASS(ipcam_config_manager_parent_class)->dispose(self);
    }
}
static void ipcam_config_manager_finalize(GObject *self)
{
    IpcamConfigManagerPrivate *priv = ipcam_config_manager_get_instance_private(IPCAM_CONFIG_MANAGER(self));
    g_hash_table_destroy(priv->conf_hash);
    g_hash_table_destroy(priv->collection);
    G_OBJECT_CLASS(ipcam_config_manager_parent_class)->finalize(self);
}
static void ipcam_config_manager_init(IpcamConfigManager *self)
{
    IpcamConfigManagerPrivate *priv = ipcam_config_manager_get_instance_private(self);
    priv->conf_hash = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    priv->collection = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
}
static void ipcam_config_manager_class_init(IpcamConfigManagerClass *klass)
{
    GObjectClass *this_class = G_OBJECT_CLASS(klass);
    this_class->dispose = &ipcam_config_manager_dispose;
    this_class->finalize = &ipcam_config_manager_finalize;
}
gboolean ipcam_config_manager_load_config(IpcamConfigManager *config_manager, const gchar *file_path)
{
    g_return_val_if_fail(IPCAM_IS_CONFIG_MANAGER(config_manager), FALSE);
    GNode *cfg = g_node_new("config");
    yaml_parser_t parser;
    gboolean ret = FALSE;

    FILE *source = fopen(file_path, "rb");
    if (source)
    {
        yaml_parser_initialize(&parser);
        yaml_parser_set_input_file(&parser, source);
        process_layer(&parser, cfg); // Recursive parsing
        yaml_parser_delete(&parser);
        fclose(source);
        IpcamConfigManagerPrivate *priv = ipcam_config_manager_get_instance_private(config_manager);
        g_node_traverse(cfg, G_PRE_ORDER, G_TRAVERSE_LEAVES, -1, to_hash, (gpointer)priv);
        ret = TRUE;
    }
    g_node_traverse(cfg, G_PRE_ORDER, G_TRAVERSE_ALL, -1, destroy, NULL);
    g_node_destroy(cfg);
    return ret;
}
void ipcam_config_manager_merge(IpcamConfigManager *config_manager, const gchar *conf_name, const gchar *conf_value)
{
    g_return_if_fail(IPCAM_IS_CONFIG_MANAGER(config_manager));
    IpcamConfigManagerPrivate *priv = ipcam_config_manager_get_instance_private(config_manager);
    sprintf(priv->key, "config:%s", conf_name);
    if (!g_hash_table_contains(priv->conf_hash, priv->key))
    {
        g_hash_table_insert(priv->conf_hash, g_strdup(priv->key), g_strdup(conf_value));
    }
}
gchar *ipcam_config_manager_get(IpcamConfigManager *config_manager, const gchar *conf_name)
{
    g_return_val_if_fail(IPCAM_IS_CONFIG_MANAGER(config_manager), NULL);
    IpcamConfigManagerPrivate *priv = ipcam_config_manager_get_instance_private(config_manager);
    sprintf(priv->key, "config:%s", conf_name);
    gchar *ret = g_hash_table_lookup(priv->conf_hash, priv->key);
    return ret;
}
static void generate_collection(gpointer key, gpointer value, gpointer user_data)
{
    IpcamConfigManagerPrivate *priv = (IpcamConfigManagerPrivate *)user_data;
    if (g_str_has_prefix(key, priv->key))
    {
        gchar *newkey = (gchar *)key + strlen(priv->key) + 1;
        g_hash_table_insert(priv->collection, g_strdup(newkey), g_strdup(value));
    }
}
GHashTable *ipcam_config_manager_get_collection(IpcamConfigManager *config_manager, const gchar *conf_name)
{
    g_return_val_if_fail(IPCAM_IS_CONFIG_MANAGER(config_manager), NULL);
    IpcamConfigManagerPrivate *priv = ipcam_config_manager_get_instance_private(config_manager);
    sprintf(priv->key, "config:%s", conf_name);
    g_hash_table_remove_all(priv->collection);
    g_hash_table_foreach(priv->conf_hash, generate_collection, priv);
    return priv->collection;
}

enum storage_flags { VAR, VAL, SEQ }; // "Store as" switch

static void process_layer(yaml_parser_t *parser, GNode *data)
{
    GNode *last_leaf = data;
    yaml_event_t event;
    int type;
    enum storage_flags storage = VAR; // mapping cannot start with VAL definition w/o VAR key
    
    do {
        yaml_parser_parse(parser, &event);
        type = event.type;
        // Parse value either as a new leaf in the mapping
        //  or as a leaf value (one of them, in case it's a sequence)
        switch (type)
        {
        case YAML_SCALAR_EVENT:
            if (storage != VAR)
            {
                last_leaf = g_node_append_data(last_leaf, g_strdup((gchar*) event.data.scalar.value));
            }
            else
            {
                last_leaf = g_node_append_data(data, g_strdup((gchar*) event.data.scalar.value));
            }
            storage ^= VAL; // Flip VAR/VAL switch for the next event
            break;
            // Sequence - all the following scalars will be appended to the last_leaf
        case YAML_SEQUENCE_START_EVENT:
            storage = SEQ;
            break;
        case YAML_SEQUENCE_END_EVENT:
            storage = VAR;
            break;
            // depth += 1
        case YAML_MAPPING_START_EVENT:
            process_layer(parser, last_leaf);
            storage ^= VAL; // Flip VAR/VAL, w/o touching SEQ
            break;
            // depth -= 1
        default:
            break;
        }
        yaml_event_delete(&event);
    } while (type != YAML_MAPPING_END_EVENT && type != YAML_STREAM_END_EVENT);
}

static gboolean to_hash(GNode *node, gpointer data) {
    IpcamConfigManagerPrivate *priv = (IpcamConfigManagerPrivate *)data;
    gchar key[PATH_MAX] = {0};
    int i = g_node_depth(node) - 1;
    int j = 0;
    gchar **array = (gchar **)g_new(gpointer, i);
    GNode *tmp = node->parent;
    while (tmp)
    {
        array[j] = g_strdup((gchar *)tmp->data);
        j++;
        tmp = tmp->parent;
    }
    for (j = i - 1; j >= 0; j--)
    {
        strcat(key, array[j]);
        if (j >= 1)
        {
            strcat(key, ":");
        }
        g_free(array[j]);
    }
    g_free(array);
    g_print("%s => %s\n", key, (gchar *)node->data);
    g_hash_table_insert(priv->conf_hash, g_strdup(key), g_strdup(node->data));

    return (FALSE);
}
static gboolean destroy(GNode *n, gpointer data)
{
    if (!G_NODE_IS_ROOT(n))
    {
        g_free(n->data);
    }
    return (FALSE);
}
