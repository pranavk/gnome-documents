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

#define GOA_API_IS_SUBJECT_TO_CHANGE
#include <goa/goa.h>
#include <grilo.h>

#include "gd-flickr-miner.h"

/** FIXME find out how to create this identifier */
#define MINER_IDENTIFIER "gd:flickr:miner:30058620-777c-47a3-a19c-a6cdf4a315c4"

#define GRILO_TARGET_PLUGIN "grl-flickr"

/* ==================== DECLARATIONS ==================== */
static void
query_flickr (GdAccountMinerJob *job,
              GError **error);

static GObject *
create_service (GdMiner *self,
                GoaObject *object);

static void
account_miner_job_browse_container (GdAccountMinerJob *job,
                                    GrlSource         *source,
                                    GrlMedia          *container);
static gboolean
account_miner_job_process_entry (GdAccountMinerJob *job,
                                 GrlMedia *entry,
                                 GError   **error);
static void
browse_container_cb (GrlSource *  source,
                     guint        operation_id,
                     GrlMedia *   media,
                     guint        remaining,
                     gpointer     user_data,
                     const GError *error);
static void
source_added_cb (GrlRegistry *registry,
                 GrlSource   *source,
                 gpointer     user_data);

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



  /* FIXME: make the path relative and find universal way */
  /* we can use load_all, since create service returns only flickr plugin */
  if (! grl_registry_load_plugin(registry,
                                  "/home/marek/local/lib64/grilo-0.2/libgrlflickr.so",
                                  &error))
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
  g_debug ("Querying flickr");

  GrlRegistry *registry;
  GList *m, *sources;
 
  registry = grl_registry_get_default ();

  /* enable asyncronous adding of grilo sources */
  /* TODO - solve possible multiple browsing of sources */
  g_signal_connect (registry, "source-added",
                    G_CALLBACK (source_added_cb), job);

  /* TODO - dont do that, do it all via source-added */
  sources = grl_plugin_get_sources (GRL_PLUGIN (job->service));

  for (m = sources; m != NULL; m = g_list_next (m))
  {
    /* TODO what to do with the error? */
    account_miner_job_browse_container (job,
                                        GRL_SOURCE (m->data),
                                        NULL);
  }
}

/* Fix me - in generialized version return GrlRegistry and
 * in create service just configure it (and return) */
static GObject *
create_service (GdMiner *self,
                GoaObject *object)
{ 
  GrlRegistry *registry;
  GrlPlugin *plugin;

  registry = grl_registry_get_default ();
  plugin = grl_registry_lookup_plugin (registry, GRILO_TARGET_PLUGIN);

  if (plugin == NULL)
    g_error ("Could not find services (grilo plugin: %s)", GRILO_TARGET_PLUGIN);

  return G_OBJECT (g_object_ref (plugin));
}

/* ==================== PRIVATE FUNCTIONS ==================== */

static gboolean
account_miner_job_process_entry (GdAccountMinerJob *job,
                                 GrlMedia *entry,
                                 GError   **error)
{
  g_debug ("Got media %s from source %s", grl_media_get_title (entry),
                                          grl_media_get_source (entry));
  /*
  GDateTime *created_time, *updated_time;
  gchar *contact_resource;
  gchar *resource = NULL;
  gchar *date, *identifier;
  const gchar *class = NULL, *id, *name;
  gboolean resource_exists, mtime_changed;
  gint64 new_mtime;

  id = zpj_skydrive_entry_get_id (entry);

  identifier = g_strdup_printf ("%swindows-live:skydrive:%s",
                                ZPJ_IS_SKYDRIVE_FOLDER (entry) ? "gd:collection:" : "",
                                id);

  // remove from the list of the previous resources
  g_hash_table_remove (job->previous_resources, identifier);

  name = zpj_skydrive_entry_get_name (entry);

  if (ZPJ_IS_SKYDRIVE_FILE (entry))
    class = gd_filename_to_rdf_type (name);
  else if (ZPJ_IS_SKYDRIVE_FOLDER (entry))
    class = "nfo:DataContainer";

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

  updated_time = zpj_skydrive_entry_get_updated_time (entry);
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

  if (ZPJ_IS_SKYDRIVE_FILE (entry))
    {
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

  gd_miner_tracker_sparql_connection_insert_or_replace_triple
    (job->connection,
     job->cancellable, error,
     job->datasource_urn, resource,
     "nie:description", zpj_skydrive_entry_get_description (entry));

  if (*error != NULL)
    goto out;

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
     job->datasource_urn, zpj_skydrive_entry_get_from_name (entry));

  if (*error != NULL)
    goto out;

  gd_miner_tracker_sparql_connection_insert_or_replace_triple
    (job->connection,
     job->cancellable, error,
     job->datasource_urn, resource,
     "nco:creator", contact_resource);
  g_free (contact_resource);

  if (*error != NULL)
    goto out;

  created_time = zpj_skydrive_entry_get_created_time (entry);
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
*/
  
  g_object_unref (entry);

  return TRUE;
}

static void
account_miner_job_browse_container (GdAccountMinerJob *job,
                                    GrlSource *source,
                                    GrlMedia  *container)
{
  g_return_if_fail (GRL_IS_SOURCE (source));
  g_return_if_fail (container == NULL || GRL_IS_MEDIA  (container));

  /* Skip public source */
  if (g_strcmp0 (grl_source_get_name (source), "Flickr") == 0) {
    g_debug ("Skipping public source");
    return;
  }

  GrlOperationOptions *ops;
  const GList *keys;
  gint op_id;

  /* get possiblly all */
  ops = grl_operation_options_new (NULL);
  keys = grl_source_supported_keys (source);

  op_id = grl_source_browse (source, container,
                             keys, ops, browse_container_cb, job);

  /* TODO use op_id to make it cancellable */

  g_object_unref (ops);
}

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

  GError *err = NULL;

  if (media != NULL)
  {
    if (GRL_IS_MEDIA_BOX (media) && source != NULL)
    {
      account_miner_job_browse_container ((GdAccountMinerJob *) user_data,
                                          source, media);
      g_object_unref (media);
    }
    else if (GRL_IS_MEDIA_IMAGE (media))
    {
      /* TODO now is process entry undefined, but if it will be
       * some kind of async, we need to handle errors somehow */
      account_miner_job_process_entry ((GdAccountMinerJob *) user_data,
                                       media, &err);

      if (err != NULL)
      {
        g_warning ("%s", err->message);
        g_error_free (err);
      }
    }
    else
    {
      /* some future extension? */
      return;
    }
  }
}

static void
source_added_cb (GrlRegistry *registry,
                 GrlSource   *source,
                 gpointer     user_data)
{
  g_debug ("New source: %s", grl_source_get_name (source));

  account_miner_job_browse_container ((GdAccountMinerJob *) user_data, source, NULL);
}
