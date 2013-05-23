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
#define MINER_VERSION 1

#define GRILO_TARGET_PLUGIN "grl-flickr"
#define GRILO_PUBLIC_SOURCE_NAME "Flickr"
#define GRILO_SOURCE_ID_PREFIX "grl-flickr-"

#define GOA_PROVIDER_TYPE "flickr"

#define POOL_WAIT_SEC 2

/* ==================== DECLARATIONS ==================== */

/*
 * Data for callbacks
 */
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

inline static const gchar*
get_goa_id (const gchar *source_id);

static void
query_flickr (GdAccountMinerJob *job,
              GError **error);

static GObject *
create_service (GdMiner *self,
                GoaObject *object);

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

static inline struct entry *create_entry (GrlMedia *media, GrlMedia *parent,
                                    GrlSource *source, struct data *data);

static inline void delete_entry (struct entry *ent);

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

  miner_class->goa_provider_type = GOA_PROVIDER_TYPE;
  miner_class->miner_identifier = MINER_IDENTIFIER;
  miner_class->version = MINER_VERSION;

  miner_class->create_service = create_service;
  miner_class->query = query_flickr;

  grl_init(NULL, NULL);
  registry = grl_registry_get_default();

  if (! grl_registry_load_plugin_by_id (registry, GRILO_TARGET_PLUGIN, &error))
  {
    g_error ("%s", error->message);
  }
}


/* ==================== "EXPORTED" FUNCTIONS ==================== */

static void
query_flickr (GdAccountMinerJob *job,
              GError **error)
{
  struct entry *ent;
  struct data  d;

  if (job->service == NULL)
  {
    g_debug ("Called query with no source to browse! Query quit.");

    /* TODO is there some domain? */
    g_set_error_literal (error, NULL, 0, "Query with NULL service");

    return;
  }

  /* data for callback functions */
  d.job     = job;
  d.entries = g_hash_table_new (NULL, NULL);

  ent = create_entry (NULL, NULL, GRL_SOURCE (job->service), &d);
  account_miner_job_browse_container (ent);

  /* Wait for pending browsings */
  /* Explanation: when calling browse, we add it's data it uses into hash.
   * When the browse is finished, we remove the data from the hash.
   * Here we are waiting for the hash to be empty => which means all browsings
   * were finished. It prevents from prematurely ending of query_flickr (since
   * browsing is asynchronous) and therefore from freeing job during the usage */
  while (1)
  {
    /* dont hurry, wait POOL_WAIT_SEC before asking for state */
    g_usleep (G_USEC_PER_SEC * POOL_WAIT_SEC);

    if (g_hash_table_size (d.entries) == 0)
    {
      g_debug ("No active browsing. Quiting query..");
      break;
    }
  }
}

/*
 * Returns source for browsing or NULL when error is reached
 * FIXME: what if source is added after this function is called?
 */
static GObject *
create_service (GdMiner *self,
                GoaObject *object)
{

  GrlRegistry *registry;
  GrlSource *retval = NULL;
  gchar *source_id = NULL;

  GoaAccount *acc = goa_object_peek_account (object);

  if (acc == NULL)
    return NULL;

  source_id = g_strdup_printf("%s%s", GRILO_SOURCE_ID_PREFIX,
                                      goa_account_get_id (acc));

  g_debug ("Looking for source %s", source_id);

  registry = grl_registry_get_default ();
  retval = grl_registry_lookup_source (registry, source_id);

  /* freeing job calls unref upon this object */
  if (retval != NULL)
    retval = g_object_ref (retval);

  if (source_id != NULL)
    g_free (source_id);

  return G_OBJECT (retval);
}


/* ==================== PRIVATE FUNCTIONS ==================== */

void
account_miner_job_browse_container (struct entry *entry)
{
  g_return_if_fail (entry != NULL);
  g_return_if_fail (entry->media == NULL || GRL_IS_MEDIA (entry->media));
  g_return_if_fail (entry->parent == NULL || GRL_IS_MEDIA (entry->parent));
  g_return_if_fail (GRL_IS_SOURCE (entry->source));

  g_debug ("Browsing container '%s' [parent: '%s', source '%s']", entry->media ?grl_media_get_title (entry->media) : "root",
                                          entry->parent ? grl_media_get_title (entry->parent) : "none",
                                          grl_source_get_name (entry->source));

  /* Skip public source */
  if (g_strcmp0 (grl_source_get_name (entry->source), GRILO_PUBLIC_SOURCE_NAME) == 0) {
    g_debug ("Skipping public source");
    delete_entry (entry); 
    return;
  }

  GrlOperationOptions *ops;
  GrlCaps *caps;
  const GList *keys;

  /* get possiblly all */
  keys = grl_source_supported_keys (entry->source);
  caps = grl_source_get_caps (entry->source, GRL_OP_BROWSE);
  ops = grl_operation_options_new (caps);

  /* FIXME make the browsing cancellable */
  grl_source_browse (entry->source, entry->media,
                     keys, ops, browse_container_cb, entry);

  g_object_unref (ops);
}

static gboolean
account_miner_job_process_entry (struct entry *entry, GError **error)
{
  g_debug ("Got %s '%s' from source '%s'", GRL_IS_MEDIA_BOX (entry->media) ? "box" : "media",
                                        grl_media_get_title (entry->media),
                                        grl_media_get_source (entry->media));

  GDateTime *created_time, *updated_time;
  gchar *contact_resource;
  gchar *resource = NULL;
  gchar *date, *identifier;
  const gchar *class = NULL, *id, *name;
  gboolean resource_exists, mtime_changed;
  gint64 new_mtime;

  GrlMedia *media = entry->media;
  GdAccountMinerJob *job = entry->data->job;

  id = grl_media_get_id (media);
  identifier = g_strdup_printf ("%sflickr:%s",
                                GRL_IS_MEDIA_BOX (media) ? "gd:collection:" : "",
                                id);

  // remove from the list of the previous resources
  g_hash_table_remove (job->previous_resources, identifier);

  name = grl_media_get_title (media);

  if (GRL_IS_MEDIA_BOX (media))
    class = "nfo:DataContainer";
  else
    class = "nmm:Photo";
  /* TODO on flickr can be videos too */

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

/*
  no changes can be done as far as I know .. or leave it here and just test for NULL..

  updated_time = grl_media_get_modification_date (media);
  new_mtime = g_date_time_to_unix (updated_time);
  mtime_changed = gd_miner_tracker_update_mtime (job->connection, new_mtime,
                                                 resource_exists, identifier, resource,
                                                 job->cancellable, error);

  if (*error != NULL)
    goto out;

  //avoid updating the DB if the media already exists and has not
  //been modified since our last run.
  //
  if (!mtime_changed)
    goto out;
*/

  //the resource changed - just set all the properties again
  gd_miner_tracker_sparql_connection_insert_or_replace_triple
    (job->connection,
     job->cancellable, error,
     job->datasource_urn, resource,
     "nie:url", grl_media_get_url (media));
     /*"nie:url", identifier); */

  if (*error != NULL)
    goto out;

      /*
  if (! GRL_IS_MEDIA_BOX (media))
    {
      gchar *parent_resource_urn, *parent_identifier;
      const gchar *parent_id, *mime;

      parent_id = zpj_skydrive_media_get_parent_id (media);
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
        */

  // insert description
  gd_miner_tracker_sparql_connection_insert_or_replace_triple
    (job->connection,
     job->cancellable, error,
     job->datasource_urn, resource,
     "nie:description", grl_media_get_description (media));

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

  contact_resource = gd_miner_tracker_utils_ensure_contact_resource
    (job->connection,
     job->cancellable, error,
     job->datasource_urn, grl_media_get_author (media));

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
  created_time = grl_media_get_creation_date (media);
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
  struct entry *parent_ent = (struct entry *) user_data;
  GError *err = NULL;

  /* Remove entry from hash if we are finished */
  if (remaining == 0)
  {
    delete_entry (parent_ent);
  }

  if (media != NULL)
  {
    ent = create_entry (media, parent_ent->media, source, parent_ent->data);

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
        err = NULL;
      }

      delete_entry (ent);
    }
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


struct entry *create_entry (GrlMedia *media, GrlMedia *parent,
                                    GrlSource *source, struct data *data)
{
    struct entry *ent;

    ent = g_slice_alloc (sizeof (struct entry));

    ent->source = source;
    ent->media  = media;
    ent->parent = (parent != NULL) ?
                    g_object_ref (parent)
                    : NULL; 
    ent->data   = data;

    g_hash_table_add (ent->data->entries, ent);

    return ent;
}

static const gchar*
get_goa_id (const gchar *source_id)
{
  /* find last occurence of - */
  return strrchr (source_id, '-');

}


