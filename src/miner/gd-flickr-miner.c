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

/** TODO: find out how to create this identifier */
#define MINER_IDENTIFIER "gd:flickr:miner:30058620-777c-47a3-a19c-a6cdf4a315c4"

#define GRL_LOG_DOMAIN_DEFAULT flickr_miner_grl_log_domain
GRL_LOG_DOMAIN_STATIC(flickr_miner_grl_log_domain);


G_DEFINE_TYPE (GdFlickrMiner, gd_flickr_miner, GD_TYPE_MINER)

static gboolean
account_miner_job_process_entry (GdAccountMinerJob *job,
                                /* ZpjSkydriveEntry *entry*/ gpointer *entry,
                                 GError **error)
{
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
  return TRUE;
}

static void
account_miner_job_traverse_folder (GdAccountMinerJob *job,
                                   const gchar *folder_id,
                                   GError **error)
{
 /* GList *entries, *l;

  entries = zpj_skydrive_list_folder_id (ZPJ_SKYDRIVE (job->service),
                                         folder_id,
                                         job->cancellable,
                                         error);
  if (*error != NULL)
    goto out;

  for (l = entries; l != NULL; l = l->next)
    {
      ZpjSkydriveEntry *entry = (ZpjSkydriveEntry *) l->data;
      const gchar *id;

      id = zpj_skydrive_entry_get_id (entry);

      if (ZPJ_IS_SKYDRIVE_FOLDER (entry))
        {
          account_miner_job_traverse_folder (job, id, error);
          if (*error != NULL)
            goto out;
        }
      else if (ZPJ_IS_SKYDRIVE_PHOTO (entry))
        continue;

      account_miner_job_process_entry (job, entry, error);

      if (*error != NULL)
        {
          g_warning ("Unable to process entry %p: %s", l->data, (*error)->message);
          g_clear_error (error);
        }
    }

 out:
  if (entries != NULL)
    g_list_free_full (entries, g_object_unref);
  */
}

static void
query_flickr (GdAccountMinerJob *job,
              GError **error)
{
  account_miner_job_traverse_folder (job,
                                     NULL,
                                     error);
}

static GObject *
create_service (GdMiner *self,
                GoaObject *object)
{
  /*
   * TODO:
   * kod jako v grl-flickru, ktery vytvori config a vlozi ho
   * do grila?? Mrkni na grilo, kdyby bylo flickr-forced, tak
   * se automaticky v grilu vytvori vsechny flickr sourcy,
   * tak je to potreba nejak ohandlovat.. Asi by bylo jeste jednodussi
   * udelat obecny grilo-miner a ten pak specializovat na flickr??
   * Uvidime.. chce to studovat. */
   /**
    * Jinak toto funguje tak, ze to vrati objekt, pres ktery se budou
    * ziskavat ty data z flickru (ten objekt se ulozi do objectu
    * GAccountMinerJob a pres ten to pak budu pouzivat
    * */

  /* use flickr-forced first
   * TODO: use configuration via GoaObject */
 /*
  GrlRegistry *registry;
  GrlMedia *service;

  registry = grl_registry_get_default();

  if (! grl_registry_load_plugin(registry, "grl-flickr", NULL))
    g_error ("Flickr Miner cannot be loaded. Cannot load flickr"
              "plugin (Grilo)");

  GrlMedia *service = grl_media_new ();

  return G_OBJECT (service);
  */
  GRL_ERROR("create_service");
  g_printerr("create_service");
}

static void
gd_flickr_miner_init (GdFlickrMiner *self)
{
  GRL_ERROR("miner_init");
  g_printerr("miner_init");

  GRL_LOG_DOMAIN_INIT(flickr_miner_grl_log_domain, "grilo");
}

static void
gd_flickr_miner_class_init (GdFlickrMinerClass *klass)
{
  GdMinerClass *miner_class = GD_MINER_CLASS (klass);

  miner_class->goa_provider_type = "flickr";
  miner_class->miner_identifier = MINER_IDENTIFIER;
  miner_class->version = 1;

  miner_class->create_service = create_service;
  miner_class->query = query_flickr;

  grl_init(NULL, NULL);
  g_critical ("flickr miner class init after grl_init!");
}
