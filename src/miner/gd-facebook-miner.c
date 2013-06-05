/* -*- Mode: C; indent-tabs-mode: nil; c-basic-offset: 2; tab-width: 2; -*- */
/*
 * Copyright (c) 2013 The GNOME Foundation.
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
 * Author: Álvaro Peña <alvaropg@gmail.com>
 */

#define GOA_API_IS_SUBJECT_TO_CHANGE
#include <goa/goa.h>
#include <gfbgraph/gfbgraph.h>
#include <gfbgraph/gfbgraph-goa-authorizer.h>

#include "gd-facebook-miner.h"

#define MINER_IDENTIFIER "gd:facebook:miner:7735c11213574dbb9a589fe6b9bad06a"
#define MINER_VERSION 1

#define GOA_PROVIDER_TYPE "facebook"

/* object functions */
static void     gd_facebook_miner_class_init    (GdFacebookMinerClass *klass);
static void     gd_facebook_miner_init          (GdFacebookMiner *miner);
/* miner functions */
static GObject *create_service                  (GdMiner *self, GoaObject *object);
static void     query_facebook                  (GdAccountMinerJob *job, GError **error);
/* private functions */
static gboolean account_miner_job_lookup_album  (GdAccountMinerJob *job, GFBGraphAlbum *album, const gchar *creator, GError **error);
static gboolean account_miner_job_process_photo (GdAccountMinerJob *job, GFBGraphPhoto *photo, const gchar *parent_identifier, const gchar *creator,GError **error);

G_DEFINE_TYPE (GdFacebookMiner, gd_facebook_miner, GD_TYPE_MINER)

static void
gd_facebook_miner_class_init (GdFacebookMinerClass *klass)
{
  GdMinerClass *miner_class = GD_MINER_CLASS (klass);

  miner_class->goa_provider_type = GOA_PROVIDER_TYPE;
  miner_class->miner_identifier = MINER_IDENTIFIER;
  miner_class->version = MINER_VERSION;

  miner_class->create_service = create_service;
  miner_class->query = query_facebook;
}

static void
gd_facebook_miner_init (GdFacebookMiner *miner)
{
}

static GObject *
create_service (GdMiner *self, GoaObject *object)
{
  GFBGraphGoaAuthorizer *authorizer;
  GError *error = NULL;

  authorizer = gfbgraph_goa_authorizer_new (object);

  if (GFBGRAPH_IS_GOA_AUTHORIZER (authorizer)) {
    g_debug ("GFBGraph GOA authorizer created");
    g_object_ref (authorizer);
  }

  gfbgraph_authorizer_refresh_authorization (GFBGRAPH_AUTHORIZER (authorizer), NULL, &error);
  if (error != NULL) {
    g_warning ("Error refreshing authorization (%d): %s", error->code, error->message);
  }

  return G_OBJECT (authorizer);
}

static void
query_facebook (GdAccountMinerJob *job, GError **error)
{
  GFBGraphUser *me;
  gchar *me_name;
  GList *albums = NULL;
  GList *album_iter = NULL;
  GError *tmp_error = NULL;

  me = gfbgraph_user_get_me (GFBGRAPH_AUTHORIZER (job->service), &tmp_error);
  if (tmp_error != NULL) {
    g_warning ("Error getting \"me\" user. Error (%d): %s", tmp_error->code, tmp_error->message);
    goto out;
  }

  g_object_get (me, "name", &me_name, NULL);

  albums = gfbgraph_user_get_albums (me, GFBGRAPH_AUTHORIZER (job->service), &tmp_error);
  if (tmp_error != NULL) {
    g_warning ("Error getting albums. Error (%d): %s", tmp_error->code, tmp_error->message);
    goto out;
  }

  album_iter = albums;
  while (album_iter) {
    GFBGraphAlbum *album;

    album = GFBGRAPH_ALBUM (album_iter->data);
    account_miner_job_lookup_album (job, album, (const gchar*) me_name, error);

    album_iter = g_list_next (album_iter);
  }

 out:
  if (tmp_error != NULL)
    g_propagate_error (error, tmp_error);

  if (albums != NULL)
    g_list_free_full (albums, g_object_unref);

  if (me_name)
    g_free (me_name);

  g_clear_object (&me);
}

/* TODO: Until GFBGraph parse the "from" node section, we require the album creator (generally the logged user) */
static gboolean
account_miner_job_lookup_album (GdAccountMinerJob *job, GFBGraphAlbum *album, const gchar *creator, GError **error)
{
  const gchar *album_id;
  gchar *album_name;
  gchar *album_description;
  const gchar *album_link;
  const gchar *album_created_time;
  gchar *identifier;
  const gchar *class = "nfo:DataContainer";
  gchar *resource = NULL;
  gboolean resource_exists;
  gchar *contact_resource;
  GList *photos = NULL;
  GList *photo_iter = NULL;

  album_id = gfbgraph_node_get_id (GFBGRAPH_NODE (album));
  album_link = gfbgraph_node_get_link (GFBGRAPH_NODE (album));
  album_created_time = gfbgraph_node_get_created_time (GFBGRAPH_NODE (album));
  g_object_get (album,
                "name", &album_name,
                "description", &album_description,
                NULL);

  identifier = g_strdup_printf ("gd:collection:facebook:%s", album_id);

  /* remove from the list of the previous resources */
  g_hash_table_remove (job->previous_resources, identifier);

  resource = gd_miner_tracker_sparql_connection_ensure_resource (job->connection,
                                                                 job->cancellable, error,
                                                                 &resource_exists,
                                                                 job->datasource_urn, identifier,
                                                                 "nfo:RemoteDataObject", class,
                                                                 NULL);

  if (*error != NULL)
    goto out;

  gd_miner_tracker_update_datasource (job->connection, job->datasource_urn,
                                      resource_exists, identifier, resource,
                                      job->cancellable, error);

  if (*error != NULL)
    goto out;

  /* TODO: Check updated time to avoid updating the photo if has not been modified since our last run */

  // insert album url
  gd_miner_tracker_sparql_connection_insert_or_replace_triple (job->connection,
                                                               job->cancellable, error,
                                                               job->datasource_urn, resource,
                                                               "nie:url", album_link);
  if (*error != NULL)
    goto out;

  // insert description
  gd_miner_tracker_sparql_connection_insert_or_replace_triple (job->connection,
                                                               job->cancellable, error,
                                                               job->datasource_urn, resource,
                                                               "nie:description", album_description);
  if (*error != NULL)
    goto out;

  // insert filename
  gd_miner_tracker_sparql_connection_insert_or_replace_triple (job->connection,
                                                               job->cancellable, error,
                                                               job->datasource_urn, resource,
                                                               "nfo:fileName", album_name);
  if (*error != NULL)
    goto out;

  gd_miner_tracker_sparql_connection_insert_or_replace_triple (job->connection,
                                                               job->cancellable, error,
                                                               job->datasource_urn, resource,
                                                               "nie:contentCreated", album_created_time);
  if (*error != NULL)
    goto out;

  contact_resource = gd_miner_tracker_utils_ensure_contact_resource (job->connection,
                                                                     job->cancellable, error,
                                                                     job->datasource_urn, creator);
  if (*error != NULL)
    goto out;

  gd_miner_tracker_sparql_connection_insert_or_replace_triple (job->connection,
                                                               job->cancellable, error,
                                                               job->datasource_urn, resource,
                                                               "nco:creator", contact_resource);
  g_free (contact_resource);
  if (*error != NULL)
    goto out;

  /* Album photos */
  photos = gfbgraph_node_get_connection_nodes (GFBGRAPH_NODE (album), GFBGRAPH_TYPE_PHOTO,
                                               GFBGRAPH_AUTHORIZER (job->service),
                                               error);
  if (*error != NULL)
    goto out;

  photo_iter = photos;
  while (photo_iter) {
    GFBGraphPhoto *photo;

    photo = GFBGRAPH_PHOTO (photo_iter->data);
    account_miner_job_process_photo (job, photo, (const gchar*) identifier, creator, error);

    photo_iter = g_list_next (photo_iter);
  }

 out:
  g_free (album_name);
  g_free (album_description);
  g_free (identifier);
  g_free (resource);

  if (photos != NULL)
    g_list_free_full (photos, g_object_unref);

  if (*error != NULL)
    return FALSE;

  return TRUE;
}

static gboolean
account_miner_job_process_photo (GdAccountMinerJob *job, GFBGraphPhoto *photo, const gchar *parent_identifier, const gchar *creator, GError **error)
{
  gchar *photo_id;
  gchar *photo_name;
  gchar *photo_source;
  gchar *photo_created_time;
  gchar *identifier;
  const gchar *class = "nmm:Photo";
  gchar *resource = NULL;
  gboolean resource_exists;
  gchar *contact_resource;
  gchar *parent_resource_urn;

  g_object_get (photo,
                "id", &photo_id,
                "name", &photo_name,
                "source", &photo_source,
                "created_time", &photo_created_time,
                NULL);

  identifier = g_strdup_printf ("facebook:photos:%s", photo_id);

  /* remove from the list of the previous resources */
  g_hash_table_remove (job->previous_resources, identifier);

  resource = gd_miner_tracker_sparql_connection_ensure_resource (job->connection,
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

  /* TODO: Check updated time to avoid updating the photo if has not been modified since our last run */

  // insert url
  gd_miner_tracker_sparql_connection_insert_or_replace_triple (job->connection,
                                                               job->cancellable, error,
                                                               job->datasource_urn, resource,
                                                               "nie:url", photo_source);
  if (*error != NULL)
    goto out;

  /* link with the album */
  parent_resource_urn = gd_miner_tracker_sparql_connection_ensure_resource (job->connection,
                                                                            job->cancellable, error,
                                                                            NULL,
                                                                            job->datasource_urn, parent_identifier,
                                                                            "nfo:RemoteDataObject", "nfo:DataContainer", NULL);
  if (*error != NULL)
    goto out;

  gd_miner_tracker_sparql_connection_insert_or_replace_triple (job->connection,
                                                               job->cancellable, error,
                                                               job->datasource_urn, resource,
                                                               "nie:isPartOf", parent_resource_urn);
  g_free (parent_resource_urn);

  if (*error != NULL)
    goto out;

  /* insert filename */
  gd_miner_tracker_sparql_connection_insert_or_replace_triple (job->connection,
                                                               job->cancellable, error,
                                                               job->datasource_urn, resource,
                                                               "nfo:fileName", photo_name);
  if (*error != NULL)
    goto out;

  /* created time */
  gd_miner_tracker_sparql_connection_insert_or_replace_triple (job->connection,
                                                               job->cancellable, error,
                                                               job->datasource_urn, resource,
                                                               "nie:contentCreated", photo_created_time);
  if (*error != NULL)
    goto out;

  /* Creator */
  contact_resource = gd_miner_tracker_utils_ensure_contact_resource (job->connection,
                                                                     job->cancellable, error,
                                                                     job->datasource_urn, creator);
  if (*error != NULL)
    goto out;

  gd_miner_tracker_sparql_connection_insert_or_replace_triple (job->connection,
                                                               job->cancellable, error,
                                                               job->datasource_urn, resource,
                                                               "nco:creator", contact_resource);
  g_free (contact_resource);
  if (*error != NULL)
    goto out;

 out:
  g_free (photo_id);
  g_free (photo_name);
  g_free (photo_source);
  g_free (photo_created_time);
  g_free (identifier);
  g_free (resource);
  g_free (contact_resource);

  if (*error != NULL)
    return FALSE;

  return TRUE;
}
