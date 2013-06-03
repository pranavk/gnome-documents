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

  authorizer = gfbgraph_goa_authorizer_new (object);

  if (GFBGRAPH_IS_GOA_AUTHORIZER (authorizer)) {
    g_debug ("GFBGraph GOA authorizer created");
    g_object_ref (authorizer);
  }

  return G_OBJECT (authorizer);
}

static void
query_facebook (GdAccountMinerJob *job, GError **error)
{
  GFBGraphUser *me;
  gchar *me_name;
  GList *albums = NULL;

  me = gfbgraph_user_get_me (GFBGRAPH_AUTHORIZER (job->service), error);
  if (*error != NULL) {
    g_warning ("Error getting \"me\" user");
    goto out;
  }

  g_object_get (me, "name", &me_name, NULL);

  albums = gfbgraph_user_get_albums (me, GFBGRAPH_AUTHORIZER (job->service), error);
  if (*error != NULL) {
    g_warning ("Error getting albums");
    goto out;
  }

  while (albums) {
    GFBGraphAlbum *album;

    album = albums->data;
    account_miner_job_lookup_album (job, album, (const gchar*) me_name, error);

    albums = g_list_next (albums);
  }

 out:
  if (me_name)
    g_free (me_name);
  g_clear_object (&me);
}

/* TODO: Until GFBGraph parse the "from" node section, we require the album creator (generally the logged user) */
static gboolean
account_miner_job_lookup_album (GdAccountMinerJob *job, GFBGraphAlbum *album, const gchar *creator, GError **error)
{
  gchar *album_id;
  gchar *album_name;
  gchar *album_description;
  gchar *album_link;
  gchar *album_created_time;
  gchar *identifier;
  const gchar *class = "nfo:DataContainer";
  gchar *resource = NULL;
  gboolean resource_exists;
  gchar *contact_resource;
  GList *photos = NULL;

  g_object_get (album,
                "id", &album_id,
                "link", &album_link,
                "name", &album_name,
                "description", &album_description,
                "created_time", &album_created_time,
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

  while (photos) {
    GFBGraphPhoto *photo;

    photo = GFBGRAPH_PHOTO (photos->data);
    account_miner_job_process_photo (job, photo, (const gchar*) identifier, creator, error);

    photos = g_list_next (photos);
  }

 out:
  g_free (album_id);
  g_free (album_name);
  g_free (album_link);
  g_free (album_description);
  g_free (album_created_time);
  g_free (identifier);
  g_free (resource);

  if (*error != NULL)
    return FALSE;

  return TRUE;
}

static gboolean
account_miner_job_process_photo (GdAccountMinerJob *job, GFBGraphPhoto *photo, const gchar *parent_identifier, const gchar *creator, GError **error)
{
  gchar *photo_id;
  gchar *photo_name;
  gchar *photo_link;
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
                "link", &photo_link,
                "created_time", &photo_created_time,
                NULL);

  identifier = g_strdup_printf ("facebook:%s", photo_id);

  /* remove from the list of the previous resources */
  g_hash_table_remove (job->previous_resources, identifier);

  resource = gd_miner_tracker_sparql_connection_ensure_resource (job->connection,
                                                                 job->cancellable, error,
                                                                 &resource_exists,
                                                                 job->datasource_urn, identifier,
                                                                 "nfo:RemoteDataObject", class, NULL);
  if (*error != NULL)
    goto out;

  // insert url
  gd_miner_tracker_sparql_connection_insert_or_replace_triple (job->connection,
                                                               job->cancellable, error,
                                                               job->datasource_urn, resource,
                                                               "nie:url", photo_link);
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
  g_free (photo_link);
  g_free (photo_created_time);
  g_free (identifier);
  g_free (resource);
  g_free (contact_resource);

  if (*error != NULL)
    return FALSE;

  return TRUE;
}
