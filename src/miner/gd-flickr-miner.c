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

struct data
{
  GHashTable        *entries;
  GdAccountMinerJob *job;
  /* Mutex ? */
};

/*
 * GrlMedia with it's source and parent and data used by callbacks
 */
struct entry {
  GrlSource *source;
  GrlMedia  *media;
  GrlMedia  *parent;
  struct data *data;
};

static void
query_flickr (GdAccountMinerJob *job,
              GError **error);

static GObject *
create_service (GdMiner *self,
                GoaObject *object);

/* FIXME -> can delete job argument since it's present in pool_data struct */
static void
account_miner_job_browse_container (struct entry *entry);

static gboolean
account_miner_job_process_entry (struct entry *entry, GError **error);

/*
static void
source_added_cb (GrlRegistry *registry, GrlSource *source, gpointer user_data);
*/

static void
browse_container_cb (GrlSource *source,
                     guint operation_id,
                     GrlMedia *media,
                     guint remaining,
                     gpointer user_data,
                     const GError *error);

static void delete_entry (struct entry *ent);

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
  miner_class->goa_provider_type = "flickr";
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

  struct entry *ent;
  struct data  d;

  /* just make sure we wont be called more times */
  if (GPOINTER_TO_INT (job->service) == 0)
    return;

  d.job = job;
  d.entries = g_hash_table_new (NULL, NULL);

  registry = grl_registry_get_default ();
  sources = grl_registry_get_sources (registry, FALSE);

  for (m = sources; m != NULL; m = g_list_next (m))
  {
    g_debug ("Got source: %s", grl_source_get_name (GRL_SOURCE (m->data)));

    ent = g_slice_alloc (sizeof (struct entry));

    ent->source = GRL_SOURCE (m->data);
    ent->media  = NULL;
    ent->parent = NULL;
    ent->data = &d;

    g_hash_table_add (d.entries, ent);

    account_miner_job_browse_container (ent);
  }

  /* Wait for pending threads */
  while (1)
  {
    // dont hurry, wait POOL_WAIT_SEC before asking for state
    g_usleep (G_USEC_PER_SEC * POOL_WAIT_SEC);

    if (g_hash_table_size (d.entries) == 0)
    {
      g_debug ("No pending job. Quiting query..");
      break;
    }
  }

  g_hash_table_destroy (d.entries);

  g_debug ("Ending query_flickr (delete me)");
}

static GObject *
create_service (GdMiner *self,
                GoaObject *object)
{
  /* only prevent from multiple calling */
  /* TODO add support for multiple calling of refresh_db (remember first account id and compare */
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

void
account_miner_job_browse_container (struct entry *entry)
{
  g_return_if_fail (entry != NULL);
  g_return_if_fail (entry->media == NULL || GRL_IS_MEDIA (entry->media));
  g_return_if_fail (entry->parent == NULL || GRL_IS_MEDIA (entry->parent));
  g_return_if_fail (GRL_IS_SOURCE (entry->source));

  g_debug ("Browsing container %s of %s (%s)", entry->media ? grl_media_get_title (entry->media) : "[root]",
                                          entry->parent ? grl_media_get_title (entry->parent) : "[root]",
                                          grl_source_get_name (entry->source));

  /* Skip public source */
  if (g_strcmp0 (grl_source_get_name (entry->source), "Flickr") == 0) {
    g_debug ("Skipping public source");
    delete_entry (entry); 
    return;
  }

  GrlOperationOptions *ops;
  const GList *keys;
  GError *err = NULL;

  /* get possiblly all */
  keys = grl_source_supported_keys (entry->source);
  ops = grl_operation_options_new (grl_source_get_caps (entry->source, GRL_OP_BROWSE));

  grl_source_browse (entry->source, entry->media,
                     keys, ops, browse_container_cb, entry);

  g_object_unref (ops);
}

static gboolean
account_miner_job_process_entry (struct entry *entry, GError **error)
{
  g_debug ("Got %s %s from source %s", GRL_IS_MEDIA_BOX (entry->media) ? "box" : "media",
                                        grl_media_get_title (entry->media),
                                        grl_media_get_source (entry->media));

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


/* ==================== Utilities ==================== */
static void
browse_container_cb (GrlSource *source,
                     guint operation_id,
                     GrlMedia *media,
                     guint remaining,
                     gpointer user_data,
                     const GError *error)
{
  if (error != NULL)
  {
    g_warning ("%s", error->message);
    return;
  }

  struct entry *ent;
  struct entry *parent_ent= (struct entry *) user_data;
  GError *err = NULL;

  if (media != NULL)
  {
    ent = g_slice_alloc (sizeof (struct entry));

    ent->source = source;
    ent->media  = media;
    ent->parent = parent_ent->media; 
    ent->data   = parent_ent->data;

    g_hash_table_add (ent->data->entries, ent);

    if (GRL_IS_MEDIA_BOX (media) && source != NULL)
    {
      account_miner_job_browse_container (ent);
    }
    else
    {
      account_miner_job_process_entry (ent, &err);

      if (err != NULL)
      {
        g_warning ("%s", err->message);
        g_error_free (err);
      }

      delete_entry (ent);
    }
  }

  /* ===== clean up ===== */
  if (remaining == 0)
  {
    delete_entry (parent_ent);
  }
}

void delete_entry (struct entry *ent)
{
  g_return_if_fail (ent != NULL);

  if (ent->media != NULL)
    g_object_unref (ent->media);
  if (ent->parent != NULL)
    g_object_unref (ent->parent);

  gpointer mem = g_hash_table_lookup (ent->data->entries, ent);

  if (mem != NULL)
  {
    g_hash_table_remove (ent->data->entries, ent);
    g_slice_free1 (sizeof (struct entry), mem);
  }
  else
  {
    g_warning ("Attempt to delete wrong entry");
  }
}
