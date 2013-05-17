/*
 * Copyright (c) 2013 Red Hat, Inc.
 *
 * Gnome Documents is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * Gnome Documents is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with Gnome Documents; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Author: Marek Chalupa <mchalupa@redhat.com>
 */

/* DEV:   ------------------------------------------------
 * to make it called in gd-miner we have to add the flickr account into
   gd_miner_refresh_db_real into doc_objects. */
/* ------------------------------------------------------- */

#define GOA_API_IS_SUBJECT_TO_CHANGE
#include <goa/goa.h>
#include <grilo.h>


#include "gd-flickr-miner.h"

/** FIXME find out how to create this identifier */
#define MINER_IDENTIFIER "gd:flickr:miner:30058620-777c-47a3-a19c-a6cdf4a315c4"

#define GRILO_TARGET_PLUGIN "grl-flickr"
#define FLICKR_MINER_MAX_BROWSE_THREADS 1
#define POOL_WAIT_SEC 3

/* ==================== DECLARATIONS ==================== */

/*
 * GrlMedia with it's source and parent
 */
struct entry {
  GrlSource *source;
  GrlMedia  *folder;
  GrlMedia  *parent;
};

struct pool_data
{
  GThreadPool       *pool;
  gboolean          active;
 // GMutex            *mutex;   /* used when manipulating with entries */
  GHashTable        *entries; /* data given to pool --> need to be freed */
  gint              sources_no;
  GdAccountMinerJob *job;
};

static void
query_flickr (GdAccountMinerJob *job,
              GError **error);

static GObject *
create_service (GdMiner *self,
                GoaObject *object);

/* FIXME -> can delete job argument since it's present in pool_data struct */
static void
account_miner_job_browse_container (GdAccountMinerJob *job,
                                    struct entry      *entry,
                                    struct pool_data  *pool);
static gboolean
account_miner_job_process_entry (GdAccountMinerJob *job,
                                 GrlMedia *entry,
                                 GError   **error);

static void
source_added_cb (GrlRegistry *registry, GrlSource *source, gpointer user_data);

static void
pool_push (gpointer data, gpointer user_data);

/* ==================== GOBJECT ==================== */

G_DEFINE_TYPE (GdFlickrMiner, gd_flickr_miner, GD_TYPE_MINER)

static void
gd_flickr_miner_init (GdFlickrMiner *self)
{
}

static void
gd_flickr_miner_class_init (GdFlickrMinerClass *klass)
{
  GdMinerClass *miner_class = GD_MINER_CLASS (klass);

  GrlRegistry *registry;
  GError *error = NULL;

  /* TODO get and assign provider type from plugin */
  miner_class->goa_provider_type = "flickr"; /* leave blank - we dont need intf to search something for us */
  miner_class->miner_identifier = MINER_IDENTIFIER;
  miner_class->version = 1;

  miner_class->create_service = create_service;
  miner_class->query = query_flickr;
  
  /* TODO unload plugins and so on */
  //miner_class->finalize = gd_flickr_miner_class_finalaze;
  
  /* TODO
   * add operation_id to allow cancellabe browsing
   */

  grl_init(NULL, NULL);

  registry = grl_registry_get_default();

  if (! grl_registry_load_plugin_by_id (registry, "grl-flickr", &error))
  {
    g_error ("Flickr Miner cannot be loaded. Cannot load flickr "
             "plugin (Grilo) :: dbg error = %s\n", error->message);
  }
  
}


/* ==================== "EXPORTED" FUNCTIONS ==================== */

static void
query_flickr (GdAccountMinerJob *job,
              GError **error)
{
  GrlRegistry *registry;
  GList *m, *sources;

  struct entry    *ent;
  struct pool_data pooldata;

  gulong sig_handler;

  if (GPOINTER_TO_INT (job->service) == 0)
    return;

  GThreadPool *p = g_thread_pool_new (pool_push, &pooldata,
                                      FLICKR_MINER_MAX_BROWSE_THREADS,
                                      /*FALSE, NULL); // FALSE ==> no errors */
                                      TRUE, error);

  if (*error != NULL)
  {
    g_warning ("Pool: %s", (*error)->message);
    return;
  }

  pooldata.pool = p;
  pooldata.active = TRUE;
 // pooldata.mutex = g_mutex_new ();
  pooldata.entries = g_hash_table_new (NULL, NULL);
  //pooldata.sources_no = GPOINTER_TO_INT (job->service);
  pooldata.job = job;

  registry = grl_registry_get_default ();

  sources = grl_registry_get_sources (registry, FALSE);

  for (m = sources; m != NULL; m = g_list_next (m))
  {
    g_debug ("Got source: %s", grl_source_get_name (GRL_SOURCE (m->data)));

    ent = g_slice_alloc (sizeof (struct entry));

    ent->source = GRL_SOURCE (m->data);
    ent->folder = NULL;
    ent->parent = NULL;

    g_hash_table_add (pooldata.entries, ent);
    //pooldata.sources_no--;

    if (g_thread_pool_push (pooldata.pool, (gpointer) ent, error) == FALSE)
    {
      /* warn but continue */
      g_warning ("Pool push: %s", (*error)->message);
      g_error_free (*error);
      *error = NULL;
    }
  }

  /* Wait for pending threads */
  while (1)
  {
    // dont hurry, wait POOL_WAIT_SEC before asking for state
    g_usleep (G_USEC_PER_SEC * POOL_WAIT_SEC);

    if (g_hash_table_size (pooldata.entries) == 0)
    {
      pooldata.active = FALSE;

      g_debug ("No pending job. Quiting query..");
      break;
    }
  }
  

  /* ==========  Clean up ========== */

  g_thread_pool_free (pooldata.pool, FALSE, TRUE);
  pooldata.pool = NULL;

  g_hash_table_destroy (pooldata.entries);

  g_debug ("Ending query_flickr");
}

static GObject *
create_service (GdMiner *self,
                GoaObject *object)
{  
  static gint s = 0;


  if (s == 0)
  {
    s = 1;
    return GINT_TO_POINTER (1);
  }
  else
    return GINT_TO_POINTER (0);
}

/* ==================== PRIVATE FUNCTIONS ==================== */
static void
pool_push (gpointer data, gpointer user_data)
{
  struct entry     *ent   = (struct entry *)      data;
  struct pool_data *pool  = (struct pool_data *) user_data;

  account_miner_job_browse_container (pool->job, ent, pool);
}


/* FIXME --> delete job argument (is in pool) */
static void
account_miner_job_browse_container (GdAccountMinerJob *job,
                                    struct entry      *entry,
                                    struct pool_data  *pool)
{
  g_return_if_fail (entry != NULL);
  g_return_if_fail (entry->folder == NULL || GRL_IS_MEDIA (entry->folder));
  g_return_if_fail (entry->parent == NULL || GRL_IS_MEDIA (entry->parent));
  g_return_if_fail (GRL_IS_SOURCE (entry->source));

  g_debug ("Browsing container %s of %s (%s)", entry->folder ? grl_media_get_title (entry->folder) : "[root]",
                                          entry->parent ? grl_media_get_title (entry->parent) : "[root]",
                                          grl_source_get_name (entry->source));

  /* Skip public source */
  if (g_strcmp0 (grl_source_get_name (entry->source), "Flickr") == 0) {
    g_debug ("Skipping public source");
    g_hash_table_remove (pool->entries, entry);
    return;
  }
 
  GrlOperationOptions *ops;
  const GList *keys;
  GError *err = NULL;
  GList *result, *m;

  GrlMedia *media;
  struct entry *ent;

  /* get possiblly all */
  keys = grl_source_supported_keys (entry->source);
  ops = grl_operation_options_new (grl_source_get_caps (entry->source, GRL_OP_BROWSE));

  result = grl_source_browse_sync (entry->source, entry->folder, keys, ops, NULL);

  for (m = result; m != NULL; m = g_list_next (m))
  {
    media = GRL_MEDIA (m->data);//grl_source_resolve_sync (source, GRL_MEDIA (m->data), keys, ops, NULL);
    account_miner_job_process_entry (pool->job, media, NULL); 

    if (GRL_IS_MEDIA_BOX (media) /* && public == FALSE */)
    {
      ent = g_slice_alloc (sizeof (struct entry));

      ent->source = entry->source;
      ent->folder = g_object_ref (media); 
      ent->parent = (entry->folder == NULL) ?
                      NULL : g_object_ref (entry->folder);
    
      g_hash_table_add (pool->entries, ent);

      account_miner_job_browse_container(pool->job, ent, pool);
      //if (g_thread_pool_push (pool->pool, ent, &err) == FALSE)
      //  g_warning ("Pooling container: %s", err->message);
    }
  }

  g_list_free_full (result, g_object_unref);

  if (entry->folder != NULL)
    g_object_unref (entry->folder);
  if (entry->parent != NULL)
    g_object_unref (entry->parent);

  gpointer mem = g_hash_table_lookup (pool->entries, entry);
  g_slice_free1 (sizeof (struct entry), mem);

  g_hash_table_remove (pool->entries, entry);


  g_object_unref (ops);
}

static gboolean
account_miner_job_process_entry (GdAccountMinerJob *job,
                                 GrlMedia *entry,
                                 GError   **error)
{
  g_debug ("Got %s %s from source %s", GRL_IS_MEDIA_BOX (entry) ? "box" : "media",
                                        grl_media_get_title (entry),
                                        grl_media_get_source (entry));
/*
  GDateTime *created_time, *updated_time;
  gchar *contact_resource;
  gchar *resource = NULL;
  gchar *date, *identifier;
  const gchar *class = NULL, *id, *name;
  gboolean resource_exists, mtime_changed;
  gint64 new_mtime;

  id = grl_media_get_id (entry);

  identifier = g_strdup_printf ("%sflickr:%s",
                                GRL_IS_MEDIA_BOX (entry) ? "gd:collection:" : "",
                                id);
  
  // remove from the list of the previous resources
  //g_hash_table_remove (job->previous_resources, identifier);

  name = grl_media_get_title (entry);


  if (GRL_IS_MEDIA_BOX (entry))
    class = "nfo:DataContainer";
  else
    class = gd_filename_to_rdf_type (name);
*/
  /*
  resource = gd_miner_tracker_sparql_connection_ensure_resource
    (job->connection,
     job->cancellable, error,
     &resource_exists,
     job->datasource_urn, identifier,
     "nfo:RemoteDataObject", class, NULL);

  if (*error != NULL)
    goto out;


  gd_miner_tracker_update_datasource (job->connection, job->datasource_urn,
                                      resource_exists, identifier, resource,
                                      job->cancellable, error);

  if (*error != NULL)
    goto out;

  updated_time = grl_media_get_modification_date (entry);
  new_mtime = g_date_time_to_unix (updated_time);
  mtime_changed = gd_miner_tracker_update_mtime (job->connection, new_mtime,
                                                 resource_exists, identifier, resource,
                                                 job->cancellable, error);

  if (*error != NULL)
    goto out;

  //avoid updating the DB if the entry already exists and has not
  //been modified since our last run.
  //
  if (!mtime_changed)
    goto out;

  //the resource changed - just set all the properties again
  gd_miner_tracker_sparql_connection_insert_or_replace_triple
    (job->connection,
     job->cancellable, error,
     job->datasource_urn, resource,
     "nie:url", identifier);

  if (*error != NULL)
    goto out;


  if (! GRL_IS_MEDIA_BOX (entry))
    { 
      g_warning ("isPartOf undefined!!");
      gchar *parent_resource_urn, *parent_identifier;
      const gchar *parent_id, *mime;

      parent_id = zpj_skydrive_entry_get_parent_id (entry);
      parent_identifier = g_strconcat ("gd:collection:windows-live:skydrive:", parent_id, NULL);
      parent_resource_urn = gd_miner_tracker_sparql_connection_ensure_resource
        (job->connection, job->cancellable, error,
         NULL,
         job->datasource_urn, parent_identifier,
         "nfo:RemoteDataObject", "nfo:DataContainer", NULL);
      g_free (parent_identifier);

      if (*error != NULL)
        goto out;

      gd_miner_tracker_sparql_connection_insert_or_replace_triple
        (job->connection,
         job->cancellable, error,
         job->datasource_urn, resource,
         "nie:isPartOf", parent_resource_urn);
      g_free (parent_resource_urn);

      if (*error != NULL)
        goto out;

      mime = gd_filename_to_mime_type (name);
      if (mime != NULL)
        {
          gd_miner_tracker_sparql_connection_insert_or_replace_triple
            (job->connection,
             job->cancellable, error,
             job->datasource_urn, resource,
             "nie:mimeType", mime);

          if (*error != NULL)
            goto out;
        }
    }

  // insert description 
  gd_miner_tracker_sparql_connection_insert_or_replace_triple
    (job->connection,
     job->cancellable, error,
     job->datasource_urn, resource,
     "nie:description", grl_media_get_description (entry));

  if (*error != NULL)
    goto out;

  // insert filename 
  gd_miner_tracker_sparql_connection_insert_or_replace_triple
    (job->connection,
     job->cancellable, error,
     job->datasource_urn, resource,
     "nfo:fileName", name);

  if (*error != NULL)
    goto out;

  // DEV: why? 
  contact_resource = gd_miner_tracker_utils_ensure_contact_resource
    (job->connection,
     job->cancellable, error,
     job->datasource_urn, grl_media_get_author (entry));

  if (*error != NULL)
    goto out;

  // insert author 
  gd_miner_tracker_sparql_connection_insert_or_replace_triple
    (job->connection,
     job->cancellable, error,
     job->datasource_urn, resource,
     "nco:creator", contact_resource);
  g_free (contact_resource);

  if (*error != NULL)
    goto out;

  // get and insert creation date 
  created_time = grl_media_get_creation_date (entry);
  date = gd_iso8601_from_timestamp (g_date_time_to_unix (created_time));
  gd_miner_tracker_sparql_connection_insert_or_replace_triple
    (job->connection,
     job->cancellable, error,
     job->datasource_urn, resource,
     "nie:contentCreated", date);
  g_free (date);

  if (*error != NULL)
    goto out;

 out:
  g_free (resource);
  g_free (identifier);

  if (*error != NULL)
    return FALSE;
  
  g_object_unref (entry);
*/
  return TRUE;
}

