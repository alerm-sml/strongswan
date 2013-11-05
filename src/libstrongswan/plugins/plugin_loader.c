/*
 * Copyright (C) 2010-2013 Tobias Brunner
 * Copyright (C) 2007 Martin Willi
 * Hochschule fuer Technik Rapperswil
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <http://www.fsf.org/copyleft/gpl.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#define _GNU_SOURCE
#include "plugin_loader.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <dlfcn.h>
#include <limits.h>
#include <stdio.h>

#include <utils/debug.h>
#include <library.h>
#include <collections/hashtable.h>
#include <collections/linked_list.h>
#include <plugins/plugin.h>
#include <utils/integrity_checker.h>

typedef struct private_plugin_loader_t private_plugin_loader_t;
typedef struct registered_feature_t registered_feature_t;
typedef struct provided_feature_t provided_feature_t;
typedef struct plugin_entry_t plugin_entry_t;

/**
 * private data of plugin_loader
 */
struct private_plugin_loader_t {

	/**
	 * public functions
	 */
	plugin_loader_t public;

	/**
	 * List of plugins, as plugin_entry_t
	 */
	linked_list_t *plugins;

	/**
	 * Hashtable for registered features, as registered_feature_t
	 */
	hashtable_t *features;

	/**
	 * Loaded features (stored in reverse order), as provided_feature_t
	 */
	linked_list_t *loaded;

	/**
	 * List of paths to search for plugins
	 */
	linked_list_t *paths;

	/**
	 * List of names of loaded plugins
	 */
	char *loaded_plugins;

	/**
	 * Statistics collected while loading features
	 */
	struct {
		/** Number of features that failed to load */
		int failed;
		/** Number of features that failed because of unmet dependencies */
		int depends;
		/** Number of features in critical plugins that failed to load */
		int critical;
	} stats;
};

/**
 * Registered plugin feature
 */
struct registered_feature_t {

	/**
	 * The registered feature
	 */
	plugin_feature_t *feature;

	/**
	 * List of plugins providing this feature, as provided_feature_t
	 */
	linked_list_t *plugins;
};

/**
 * Hash a registered feature
 */
static bool registered_feature_hash(registered_feature_t *this)
{
	return plugin_feature_hash(this->feature);
}

/**
 * Compare two registered features
 */
static bool registered_feature_equals(registered_feature_t *a,
									  registered_feature_t *b)
{
	return plugin_feature_equals(a->feature, b->feature);
}

/**
 * Feature as provided by a plugin
 */
struct provided_feature_t {

	/**
	 * Plugin providing the feature
	 */
	plugin_entry_t *entry;

	/**
	 * FEATURE_REGISTER or FEATURE_CALLBACK entry
	 */
	plugin_feature_t *reg;

	/**
	 * The provided feature (followed by dependencies)
	 */
	plugin_feature_t *feature;

	/**
	 * Maximum number of dependencies (following feature)
	 */
	int dependencies;

	/**
	 * TRUE if currently loading this feature (to prevent loops)
	 */
	bool loading;

	/**
	 * TRUE if feature loaded
	 */
	bool loaded;

	/**
	 * TRUE if feature failed to load
	 */
	bool failed;
};

/**
 * Entry for a plugin
 */
struct plugin_entry_t {

	/**
	 * Plugin instance
	 */
	plugin_t *plugin;

	/**
	 * TRUE, if the plugin is marked as critical
	 */
	bool critical;

	/**
	 * dlopen handle, if in separate lib
	 */
	void *handle;

	/**
	 * List of features, as provided_feature_t
	 */
	linked_list_t *features;
};

/**
 * Destroy a plugin entry
 */
static void plugin_entry_destroy(plugin_entry_t *entry)
{
	DESTROY_IF(entry->plugin);
	if (entry->handle)
	{
		dlclose(entry->handle);
	}
	entry->features->destroy(entry->features);
	free(entry);
}

/**
 * Wrapper for static plugin features
 */
typedef struct {

	/**
	 * Implements plugin_t interface
	 */
	plugin_t public;

	/**
	 * Name of the module registering these features
	 */
	char *name;

	/**
	 * Static plugin features
	 */
	plugin_feature_t *features;

	/**
	 * Number of plugin features
	 */
	int count;

} static_features_t;

METHOD(plugin_t, get_static_name, char*,
	static_features_t *this)
{
	return this->name;
}

METHOD(plugin_t, get_static_features, int,
	static_features_t *this, plugin_feature_t *features[])
{
	*features = this->features;
	return this->count;
}

METHOD(plugin_t, static_destroy, void,
	static_features_t *this)
{
	free(this->features);
	free(this->name);
	free(this);
}

/**
 * Create a wrapper around static plugin features.
 */
static plugin_t *static_features_create(const char *name,
										plugin_feature_t features[], int count)
{
	static_features_t *this;

	INIT(this,
		.public = {
			.get_name = _get_static_name,
			.get_features = _get_static_features,
			.destroy = _static_destroy,
		},
		.name = strdup(name),
		.features = calloc(count, sizeof(plugin_feature_t)),
		.count = count,
	);

	memcpy(this->features, features, sizeof(plugin_feature_t) * count);

	return &this->public;
}

/**
 * create a plugin
 * returns: NOT_FOUND, if the constructor was not found
 *          FAILED, if the plugin could not be constructed
 */
static status_t create_plugin(private_plugin_loader_t *this, void *handle,
							  char *name, bool integrity, bool critical,
							  plugin_entry_t **entry)
{
	char create[128];
	plugin_t *plugin;
	plugin_constructor_t constructor;

	if (snprintf(create, sizeof(create), "%s_plugin_create",
				 name) >= sizeof(create))
	{
		return FAILED;
	}
	translate(create, "-", "_");
	constructor = dlsym(handle, create);
	if (constructor == NULL)
	{
		return NOT_FOUND;
	}
	if (integrity && lib->integrity)
	{
		if (!lib->integrity->check_segment(lib->integrity, name, constructor))
		{
			DBG1(DBG_LIB, "plugin '%s': failed segment integrity test", name);
			return FAILED;
		}
		DBG1(DBG_LIB, "plugin '%s': passed file and segment integrity tests",
			 name);
	}
	plugin = constructor();
	if (plugin == NULL)
	{
		DBG1(DBG_LIB, "plugin '%s': failed to load - %s returned NULL", name,
			 create);
		return FAILED;
	}
	INIT(*entry,
		.plugin = plugin,
		.critical = critical,
		.features = linked_list_create(),
	);
	DBG2(DBG_LIB, "plugin '%s': loaded successfully", name);
	return SUCCESS;
}

/**
 * load a single plugin
 */
static plugin_entry_t *load_plugin(private_plugin_loader_t *this, char *name,
								   char *file, bool critical)
{
	plugin_entry_t *entry;
	void *handle;

	switch (create_plugin(this, RTLD_DEFAULT, name, FALSE, critical, &entry))
	{
		case SUCCESS:
			this->plugins->insert_last(this->plugins, entry);
			return entry;
		case NOT_FOUND:
			if (file)
			{	/* try to load the plugin from a file */
				break;
			}
			/* fall-through */
		default:
			return NULL;
	}
	if (lib->integrity)
	{
		if (!lib->integrity->check_file(lib->integrity, name, file))
		{
			DBG1(DBG_LIB, "plugin '%s': failed file integrity test of '%s'",
				 name, file);
			return NULL;
		}
	}
	handle = dlopen(file, RTLD_LAZY);
	if (handle == NULL)
	{
		DBG1(DBG_LIB, "plugin '%s' failed to load: %s", name, dlerror());
		return NULL;
	}
	if (create_plugin(this, handle, name, TRUE, critical, &entry) != SUCCESS)
	{
		dlclose(handle);
		return NULL;
	}
	entry->handle = handle;
	this->plugins->insert_last(this->plugins, entry);
	return entry;
}

/**
 * Convert enumerated provided_feature_t to plugin_feature_t
 */
static bool feature_filter(void *null, provided_feature_t **provided,
						   plugin_feature_t **feature)
{
	*feature = (*provided)->feature;
	return (*provided)->loaded;
}

/**
 * Convert enumerated entries to plugin_t
 */
static bool plugin_filter(void *null, plugin_entry_t **entry, plugin_t **plugin,
						  void *in, linked_list_t **list)
{
	plugin_entry_t *this = *entry;

	*plugin = this->plugin;
	if (list)
	{
		enumerator_t *features;
		features = enumerator_create_filter(
							this->features->create_enumerator(this->features),
							(void*)feature_filter, NULL, NULL);
		*list = linked_list_create_from_enumerator(features);
	}
	return TRUE;
}

METHOD(plugin_loader_t, create_plugin_enumerator, enumerator_t*,
	private_plugin_loader_t *this)
{
	return enumerator_create_filter(
							this->plugins->create_enumerator(this->plugins),
							(void*)plugin_filter, NULL, NULL);
}

METHOD(plugin_loader_t, has_feature, bool,
	private_plugin_loader_t *this, plugin_feature_t feature)
{
	enumerator_t *plugins, *features;
	plugin_t *plugin;
	linked_list_t *list;
	plugin_feature_t *current;
	bool found = FALSE;

	plugins = create_plugin_enumerator(this);
	while (plugins->enumerate(plugins, &plugin, &list))
	{
		features = list->create_enumerator(list);
		while (features->enumerate(features, &current))
		{
			if (plugin_feature_matches(&feature, current))
			{
				found = TRUE;
				break;
			}
		}
		features->destroy(features);
		list->destroy(list);
	}
	plugins->destroy(plugins);

	return found;
}

/**
 * Create a list of the names of all loaded plugins
 */
static char* loaded_plugins_list(private_plugin_loader_t *this)
{
	int buf_len = 128, len = 0;
	char *buf, *name;
	enumerator_t *enumerator;
	plugin_t *plugin;

	buf = malloc(buf_len);
	buf[0] = '\0';
	enumerator = create_plugin_enumerator(this);
	while (enumerator->enumerate(enumerator, &plugin, NULL))
	{
		name = plugin->get_name(plugin);
		if (len + (strlen(name) + 1) >= buf_len)
		{
			buf_len <<= 1;
			buf = realloc(buf, buf_len);
		}
		len += snprintf(&buf[len], buf_len - len, "%s ", name);
	}
	enumerator->destroy(enumerator);
	if (len > 0 && buf[len - 1] == ' ')
	{
		buf[len - 1] = '\0';
	}
	return buf;
}

/**
 * Check if a plugin is already loaded
 */
static bool plugin_loaded(private_plugin_loader_t *this, char *name)
{
	enumerator_t *enumerator;
	bool found = FALSE;
	plugin_t *plugin;

	enumerator = create_plugin_enumerator(this);
	while (enumerator->enumerate(enumerator, &plugin, NULL))
	{
		if (streq(plugin->get_name(plugin), name))
		{
			found = TRUE;
			break;
		}
	}
	enumerator->destroy(enumerator);
	return found;
}

/**
 * Forward declaration
 */
static void load_provided(private_plugin_loader_t *this,
						  provided_feature_t *provided,
						  int level);

/**
 * Used to find a loaded feature
 */
static bool is_feature_loaded(provided_feature_t *item)
{
	return item->loaded;
}

/**
 * Used to find a loadable feature
 */
static bool is_feature_loadable(provided_feature_t *item)
{
	return !item->loading && !item->loaded && !item->failed;
}

/**
 * Find a loaded and matching feature
 */
static bool loaded_feature_matches(registered_feature_t *a,
								   registered_feature_t *b)
{
	if (plugin_feature_matches(a->feature, b->feature))
	{
		return b->plugins->find_first(b->plugins, (void*)is_feature_loaded,
									  NULL) == SUCCESS;
	}
	return FALSE;
}

/**
 * Find a loadable module that equals the requested feature
 */
static bool loadable_feature_equals(registered_feature_t *a,
									registered_feature_t *b)
{
	if (plugin_feature_equals(a->feature, b->feature))
	{
		return b->plugins->find_first(b->plugins, (void*)is_feature_loadable,
									  NULL) == SUCCESS;
	}
	return FALSE;
}

/**
 * Find a loadable module that matches the requested feature
 */
static bool loadable_feature_matches(registered_feature_t *a,
									 registered_feature_t *b)
{
	if (plugin_feature_matches(a->feature, b->feature))
	{
		return b->plugins->find_first(b->plugins, (void*)is_feature_loadable,
									  NULL) == SUCCESS;
	}
	return FALSE;
}

/**
 * Returns a compatible plugin feature for the given depencency
 */
static bool find_compatible_feature(private_plugin_loader_t *this,
									plugin_feature_t *dependency)
{
	registered_feature_t *feature, lookup = {
		.feature = dependency,
	};

	feature = this->features->get_match(this->features, &lookup,
									   (void*)loaded_feature_matches);
	return feature != NULL;
}

/**
 * Load a registered plugin feature
 */
static void load_registered(private_plugin_loader_t *this,
							registered_feature_t *registered,
							int level)
{
	enumerator_t *enumerator;
	provided_feature_t *provided;

	enumerator = registered->plugins->create_enumerator(registered->plugins);
	while (enumerator->enumerate(enumerator, &provided))
	{
		load_provided(this, provided, level);
	}
	enumerator->destroy(enumerator);
}

/**
 * Try to load dependencies of the given feature
 */
static bool load_dependencies(private_plugin_loader_t *this,
							  provided_feature_t *provided,
							  int level)
{
	registered_feature_t *registered, lookup;
	int indent = level * 2;
	int i;

	/* first entry is provided feature, followed by dependencies */
	for (i = 1; i < provided->dependencies; i++)
	{
		if (provided->feature[i].kind != FEATURE_DEPENDS &&
			provided->feature[i].kind != FEATURE_SDEPEND)
		{	/* end of dependencies */
			break;
		}

		/* we load the feature even if a compatible one is already loaded,
		 * otherwise e.g. a specific database implementation loaded before
		 * another might cause a plugin feature loaded in-between to fail */
		lookup.feature = &provided->feature[i];
		do
		{	/* prefer an exactly matching feature, could be omitted but
			 * results in a more predictable behavior */
			registered = this->features->get_match(this->features,
										 &lookup,
										 (void*)loadable_feature_equals);
			if (!registered)
			{	/* try fuzzy matching */
				registered = this->features->get_match(this->features,
										 &lookup,
										 (void*)loadable_feature_matches);
			}
			if (registered)
			{
				load_registered(this, registered, level);
			}
			/* we could stop after finding one but for dependencies like
			 * DB_ANY it might be needed to load all matching features */
		}
		while (registered);

		if (!find_compatible_feature(this, &provided->feature[i]))
		{
			char *name, *provide, *depend;
			bool soft = provided->feature[i].kind == FEATURE_SDEPEND;

			name = provided->entry->plugin->get_name(provided->entry->plugin);
			provide = plugin_feature_get_string(&provided->feature[0]);
			depend = plugin_feature_get_string(&provided->feature[i]);
			if (soft)
			{
				DBG3(DBG_LIB, "%*sfeature %s in plugin '%s' has unmet soft "
					 "dependency: %s", indent, "", provide, name, depend);
			}
			else if (provided->entry->critical)
			{
				DBG1(DBG_LIB, "feature %s in critical plugin '%s' has unmet "
					 "dependency: %s", provide, name, depend);
			}
			else
			{
				DBG2(DBG_LIB, "feature %s in plugin '%s' has unmet dependency: "
					 "%s", provide, name, depend);
			}
			free(provide);
			free(depend);
			if (soft)
			{	/* it's ok if we can't resolve soft dependencies */
				continue;
			}
			return FALSE;
		}
	}
	return TRUE;
}

/**
 * Load registered plugin features
 */
static void load_feature(private_plugin_loader_t *this,
						 provided_feature_t *provided,
						 int level)
{
	if (load_dependencies(this, provided, level))
	{
		char *name, *provide;

		if (plugin_feature_load(provided->entry->plugin, provided->feature,
								provided->reg))
		{
			provided->loaded = TRUE;
			/* insert first so we can unload the features in reverse order */
			this->loaded->insert_first(this->loaded, provided);
			return;
		}

		name = provided->entry->plugin->get_name(provided->entry->plugin);
		provide = plugin_feature_get_string(&provided->feature[0]);
		if (provided->entry->critical)
		{
			DBG1(DBG_LIB, "feature %s in critical plugin '%s' failed to load",
				 provide, name);
		}
		else
		{
			DBG2(DBG_LIB, "feature %s in plugin '%s' failed to load",
				 provide, name);
		}
		free(provide);
	}
	else
	{	/* TODO: we could check the current level and set a different flag when
		 * being loaded as dependency. If there are loops there is a chance the
		 * feature can be loaded later when loading the feature directly. */
		this->stats.depends++;
	}
	provided->failed = TRUE;
	this->stats.critical += provided->entry->critical ? 1 : 0;
	this->stats.failed++;
}

/**
 * Load a provided feature
 */
static void load_provided(private_plugin_loader_t *this,
						  provided_feature_t *provided,
						  int level)
{
	char *name, *provide;
	int indent = level * 2;

	if (provided->loaded || provided->failed)
	{
		return;
	}
	name = provided->entry->plugin->get_name(provided->entry->plugin);
	provide = plugin_feature_get_string(provided->feature);
	if (provided->loading)
	{	/* prevent loop */
		DBG3(DBG_LIB, "%*sloop detected while loading %s in plugin '%s'",
			 indent, "", provide, name);
		free(provide);
		return;
	}
	DBG3(DBG_LIB, "%*sloading feature %s in plugin '%s'",
		 indent, "", provide, name);
	free(provide);

	provided->loading = TRUE;
	load_feature(this, provided, level + 1);
	provided->loading = FALSE;
}

/**
 * Load registered plugin features
 */
static void load_features(private_plugin_loader_t *this)
{
	enumerator_t *enumerator, *inner;
	plugin_entry_t *plugin;
	provided_feature_t *provided;

	/* we do this in plugin order to allow implicit dependencies to be resolved
	 * by reordering plugins */
	enumerator = this->plugins->create_enumerator(this->plugins);
	while (enumerator->enumerate(enumerator, &plugin))
	{
		inner = plugin->features->create_enumerator(plugin->features);
		while (inner->enumerate(inner, &provided))
		{
			load_provided(this, provided, 0);
		}
		inner->destroy(inner);
	}
	enumerator->destroy(enumerator);
}

/**
 * Register plugin features provided by the given plugin
 */
static void register_features(private_plugin_loader_t *this,
							  plugin_entry_t *entry)
{
	plugin_feature_t *feature, *reg;
	registered_feature_t *registered, lookup;
	provided_feature_t *provided;
	int count, i;

	if (!entry->plugin->get_features)
	{	/* feature interface not supported */
		DBG1(DBG_LIB, "plugin '%s' does not provide features, deprecated",
			 entry->plugin->get_name(entry->plugin));
		return;
	}
	reg = NULL;
	count = entry->plugin->get_features(entry->plugin, &feature);
	for (i = 0; i < count; i++)
	{
		switch (feature->kind)
		{
			case FEATURE_PROVIDE:
				lookup.feature = feature;
				registered = this->features->get(this->features, &lookup);
				if (!registered)
				{
					INIT(registered,
						.feature = feature,
						.plugins = linked_list_create(),
					);
					this->features->put(this->features, registered, registered);
				}
				INIT(provided,
					.entry = entry,
					.feature = feature,
					.reg = reg,
					.dependencies = count - i,
				);
				registered->plugins->insert_last(registered->plugins,
												 provided);
				entry->features->insert_last(entry->features, provided);
				break;
			case FEATURE_REGISTER:
			case FEATURE_CALLBACK:
				reg = feature;
				break;
			default:
				break;
		}
		feature++;
	}
}

/**
 * Unregister a plugin feature
 */
static void unregister_feature(private_plugin_loader_t *this,
							   provided_feature_t *provided)
{
	registered_feature_t *registered, lookup;

	lookup.feature = provided->feature;
	registered = this->features->get(this->features, &lookup);
	if (registered)
	{
		registered->plugins->remove(registered->plugins, provided, NULL);
		if (registered->plugins->get_count(registered->plugins) == 0)
		{
			this->features->remove(this->features, &lookup);
			registered->plugins->destroy(registered->plugins);
			free(registered);
		}
		else if (registered->feature == provided->feature)
		{	/* update feature in case the providing plugin gets unloaded */
			provided_feature_t *first;

			registered->plugins->get_first(registered->plugins, (void**)&first);
			registered->feature = first->feature;
		}
	}
	free(provided);
}

/**
 * Unregister plugin features
 */
static void unregister_features(private_plugin_loader_t *this,
								plugin_entry_t *entry)
{
	provided_feature_t *provided;
	enumerator_t *enumerator;

	enumerator = entry->features->create_enumerator(entry->features);
	while (enumerator->enumerate(enumerator, &provided))
	{
		entry->features->remove_at(entry->features, enumerator);
		unregister_feature(this, provided);
	}
	enumerator->destroy(enumerator);
}

/**
 * Remove plugins we were not able to load any plugin features from.
 */
static void purge_plugins(private_plugin_loader_t *this)
{
	enumerator_t *enumerator;
	plugin_entry_t *entry;

	enumerator = this->plugins->create_enumerator(this->plugins);
	while (enumerator->enumerate(enumerator, &entry))
	{
		if (!entry->plugin->get_features)
		{	/* feature interface not supported */
			continue;
		}
		if (entry->features->find_first(entry->features,
									(void*)is_feature_loaded, NULL) != SUCCESS)
		{
			DBG2(DBG_LIB, "unloading plugin '%s' without loaded features",
				 entry->plugin->get_name(entry->plugin));
			this->plugins->remove_at(this->plugins, enumerator);
			unregister_features(this, entry);
			plugin_entry_destroy(entry);
		}
	}
	enumerator->destroy(enumerator);
}

METHOD(plugin_loader_t, add_static_features, void,
	private_plugin_loader_t *this, const char *name,
	plugin_feature_t features[], int count, bool critical)
{
	plugin_entry_t *entry;
	plugin_t *plugin;

	plugin = static_features_create(name, features, count);

	INIT(entry,
		.plugin = plugin,
		.critical = critical,
		.features = linked_list_create(),
	);
	this->plugins->insert_last(this->plugins, entry);
	register_features(this, entry);
}

/**
 * Tries to find the plugin with the given name in the given path.
 */
static bool find_plugin(char *path, char *name, char *buf, char **file)
{
	struct stat stb;

	if (path && snprintf(buf, PATH_MAX, "%s/libstrongswan-%s.so",
						 path, name) < PATH_MAX)
	{
		if (stat(buf, &stb) == 0)
		{
			*file = buf;
			return TRUE;
		}
	}
	return FALSE;
}

METHOD(plugin_loader_t, load_plugins, bool,
	private_plugin_loader_t *this, char *list)
{
	enumerator_t *enumerator;
	char *default_path = NULL, *token;
	bool critical_failed = FALSE;

#ifdef PLUGINDIR
	default_path = PLUGINDIR;
#endif /* PLUGINDIR */

	enumerator = enumerator_create_token(list, " ", " ");
	while (!critical_failed && enumerator->enumerate(enumerator, &token))
	{
		plugin_entry_t *entry;
		bool critical = FALSE;
		char buf[PATH_MAX], *file = NULL;
		int len;

		token = strdup(token);
		len = strlen(token);
		if (token[len-1] == '!')
		{
			critical = TRUE;
			token[len-1] = '\0';
		}
		if (plugin_loaded(this, token))
		{
			free(token);
			continue;
		}
		if (this->paths)
		{
			this->paths->find_first(this->paths, (void*)find_plugin, NULL,
									token, buf, &file);
		}
		if (!file)
		{
			find_plugin(default_path, token, buf, &file);
		}
		entry = load_plugin(this, token, file, critical);
		if (entry)
		{
			register_features(this, entry);
		}
		else if (critical)
		{
			critical_failed = TRUE;
			DBG1(DBG_LIB, "loading critical plugin '%s' failed", token);
		}
		free(token);
	}
	enumerator->destroy(enumerator);
	if (!critical_failed)
	{
		load_features(this);
		if (this->stats.critical > 0)
		{
			critical_failed = TRUE;
			DBG1(DBG_LIB, "failed to load %d critical plugin feature%s",
				 this->stats.critical, this->stats.critical == 1 ? "" : "s");
		}
		/* unload plugins that we were not able to load any features for */
		purge_plugins(this);
	}
	if (!critical_failed)
	{
		free(this->loaded_plugins);
		this->loaded_plugins = loaded_plugins_list(this);
	}
	return !critical_failed;
}

/**
 * Unload plugin features, they are registered in reverse order
 */
static void unload_features(private_plugin_loader_t *this)
{
	enumerator_t *enumerator;
	provided_feature_t *provided;
	plugin_entry_t *entry;

	enumerator = this->loaded->create_enumerator(this->loaded);
	while (enumerator->enumerate(enumerator, &provided))
	{
		entry = provided->entry;
		plugin_feature_unload(entry->plugin, provided->feature, provided->reg);
		this->loaded->remove_at(this->loaded, enumerator);
		entry->features->remove(entry->features, provided, NULL);
		unregister_feature(this, provided);
	}
	enumerator->destroy(enumerator);
}

METHOD(plugin_loader_t, unload, void,
	private_plugin_loader_t *this)
{
	plugin_entry_t *entry;

	/* unload features followed by plugins, in reverse order */
	unload_features(this);
	while (this->plugins->remove_last(this->plugins, (void**)&entry) == SUCCESS)
	{
		if (lib->leak_detective)
		{	/* keep handle to report leaks properly */
			entry->handle = NULL;
		}
		unregister_features(this, entry);
		plugin_entry_destroy(entry);
	}
	free(this->loaded_plugins);
	this->loaded_plugins = NULL;
	memset(&this->stats, 0, sizeof(this->stats));
}

METHOD(plugin_loader_t, add_path, void,
	private_plugin_loader_t *this, char *path)
{
	if (!this->paths)
	{
		this->paths = linked_list_create();
	}
	this->paths->insert_last(this->paths, strdupnull(path));
}

/**
 * Reload a plugin by name, NULL for all
 */
static u_int reload_by_name(private_plugin_loader_t *this, char *name)
{
	u_int reloaded = 0;
	enumerator_t *enumerator;
	plugin_t *plugin;

	enumerator = create_plugin_enumerator(this);
	while (enumerator->enumerate(enumerator, &plugin, NULL))
	{
		if (name == NULL || streq(name, plugin->get_name(plugin)))
		{
			if (plugin->reload && plugin->reload(plugin))
			{
				DBG2(DBG_LIB, "reloaded configuration of '%s' plugin",
					 plugin->get_name(plugin));
				reloaded++;
			}
		}
	}
	enumerator->destroy(enumerator);
	return reloaded;
}

METHOD(plugin_loader_t, reload, u_int,
	private_plugin_loader_t *this, char *list)
{
	u_int reloaded = 0;
	enumerator_t *enumerator;
	char *name;

	if (list == NULL)
	{
		return reload_by_name(this, NULL);
	}
	enumerator = enumerator_create_token(list, " ", "");
	while (enumerator->enumerate(enumerator, &name))
	{
		reloaded += reload_by_name(this, name);
	}
	enumerator->destroy(enumerator);
	return reloaded;
}

METHOD(plugin_loader_t, loaded_plugins, char*,
	private_plugin_loader_t *this)
{
	return this->loaded_plugins ?: "";
}

METHOD(plugin_loader_t, status, void,
	private_plugin_loader_t *this, level_t level)
{
	if (this->loaded_plugins)
	{
		dbg(DBG_LIB, level, "loaded plugins: %s", this->loaded_plugins);

		if (this->stats.failed)
		{
			dbg(DBG_LIB, level, "unable to load %d plugin feature%s (%d due to "
				"unmet dependencies)", this->stats.failed,
				this->stats.failed == 1 ? "" : "s", this->stats.depends);
		}
	}
}

METHOD(plugin_loader_t, destroy, void,
	private_plugin_loader_t *this)
{
	unload(this);
	this->features->destroy(this->features);
	this->loaded->destroy(this->loaded);
	this->plugins->destroy(this->plugins);
	DESTROY_FUNCTION_IF(this->paths, free);
	free(this->loaded_plugins);
	free(this);
}

/*
 * see header file
 */
plugin_loader_t *plugin_loader_create()
{
	private_plugin_loader_t *this;

	INIT(this,
		.public = {
			.add_static_features = _add_static_features,
			.load = _load_plugins,
			.add_path = _add_path,
			.reload = _reload,
			.unload = _unload,
			.create_plugin_enumerator = _create_plugin_enumerator,
			.has_feature = _has_feature,
			.loaded_plugins = _loaded_plugins,
			.status = _status,
			.destroy = _destroy,
		},
		.plugins = linked_list_create(),
		.loaded = linked_list_create(),
		.features = hashtable_create(
							(hashtable_hash_t)registered_feature_hash,
							(hashtable_equals_t)registered_feature_equals, 64),
	);

	return &this->public;
}

/*
 * See header
 */
void plugin_loader_add_plugindirs(char *basedir, char *plugins)
{
	enumerator_t *enumerator;
	char *name, path[PATH_MAX], dir[64];

	enumerator = enumerator_create_token(plugins, " ", "");
	while (enumerator->enumerate(enumerator, &name))
	{
		snprintf(dir, sizeof(dir), "%s", name);
		translate(dir, "-", "_");
		snprintf(path, sizeof(path), "%s/%s/.libs", basedir, dir);
		lib->plugins->add_path(lib->plugins, path);
	}
	enumerator->destroy(enumerator);
}
