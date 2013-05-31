/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/*
 * Copyright (c) 2012 Red Hat, Inc.
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
 * Author: Cosimo Cecchi <cosimoc@redhat.com>
 * Author: Jasper St. Pierre <jstpierre@mecheye.net>
 *
 */

#include "config.h"

#include "gd-miner.h"

G_DEFINE_TYPE (GdMiner, gd_miner, G_TYPE_OBJECT)

struct _GdMinerPrivate {
  GoaClient *client;
  GError *client_error;

  TrackerSparqlConnection *connection;

  GCancellable *cancellable;
  GSimpleAsyncResult *result;

  GList *pending_jobs;

  gchar *display_name;
};

static void
gd_account_miner_job_free (GdAccountMinerJob *job)
{
  if (job->miner_cancellable_id != 0)
    g_cancellable_disconnect (job->miner->priv->cancellable,
                              job->miner_cancellable_id);

  g_clear_object (&job->service);
  g_clear_object (&job->miner);
  g_clear_object (&job->account);
  g_clear_object (&job->async_result);

  g_free (job->datasource_urn);
  g_free (job->root_element_urn);

  g_hash_table_unref (job->previous_resources);

  g_slice_free (GdAccountMinerJob, job);
}

static void
gd_miner_dispose (GObject *object)
{
  GdMiner *self = GD_MINER (object);

  if (self->priv->pending_jobs != NULL)
    {
      g_list_free_full (self->priv->pending_jobs,
                        (GDestroyNotify) gd_account_miner_job_free);
      self->priv->pending_jobs = NULL;
    }

  g_clear_object (&self->priv->client);
  g_clear_object (&self->priv->connection);
  g_clear_object (&self->priv->cancellable);
  g_clear_object (&self->priv->result);

  g_free (self->priv->display_name);
  g_clear_error (&self->priv->client_error);

  G_OBJECT_CLASS (gd_miner_parent_class)->dispose (object);
}

static void
gd_miner_init_goa (GdMiner *self)
{
  GoaAccount *account;
  GoaObject *object;
  const gchar *provider_type;
  GList *accounts, *l;
  GdMinerClass *miner_class = GD_MINER_GET_CLASS (self);

  self->priv->client = goa_client_new_sync (NULL, &self->priv->client_error);

  if (self->priv->client_error != NULL)
    {
      g_critical ("Unable to create GoaClient: %s - indexing for %s will not work",
                  self->priv->client_error->message, miner_class->goa_provider_type);
      return;
    }

  accounts = goa_client_get_accounts (self->priv->client);
  for (l = accounts; l != NULL; l = l->next)
    {
      object = l->data;

      account = goa_object_peek_account (object);
      if (account == NULL)
        continue;

      provider_type = goa_account_get_provider_type (account);
      if (g_strcmp0 (provider_type, miner_class->goa_provider_type) == 0)
        {
          g_free (self->priv->display_name);
          self->priv->display_name = goa_account_dup_provider_name (account);
          break;
        }
    }

  g_list_free_full (accounts, g_object_unref);
}

static void
gd_miner_constructed (GObject *obj)
{
  GdMiner *self = GD_MINER (obj);

  G_OBJECT_CLASS (gd_miner_parent_class)->constructed (obj);

  gd_miner_init_goa (self);
}

static void
gd_miner_init (GdMiner *self)
{
  self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self, GD_TYPE_MINER, GdMinerPrivate);
  self->priv->display_name = g_strdup ("");
}

static void
gd_miner_class_init (GdMinerClass *klass)
{
  GObjectClass *oclass = G_OBJECT_CLASS (klass);

  oclass->constructed = gd_miner_constructed;
  oclass->dispose = gd_miner_dispose;

  g_type_class_add_private (klass, sizeof (GdMinerPrivate));
}

static void
gd_miner_complete_error (GdMiner *self,
                         GError *error)
{
  g_assert (self->priv->result != NULL);

  g_simple_async_result_take_error (self->priv->result, error);
  g_simple_async_result_complete_in_idle (self->priv->result);
}

static void
gd_miner_check_pending_jobs (GdMiner *self)
{
  if (g_list_length (self->priv->pending_jobs) == 0)
    g_simple_async_result_complete_in_idle (self->priv->result);
}

static void
gd_account_miner_job_ensure_datasource (GdAccountMinerJob *job,
                                        GError **error)
{
  GString *datasource_insert;
  GdMinerClass *klass = GD_MINER_GET_CLASS (job->miner);

  datasource_insert = g_string_new (NULL);
  g_string_append_printf (datasource_insert,
                          "INSERT OR REPLACE INTO <%s> {"
                          "  <%s> a nie:DataSource ; nao:identifier \"%s\" . "
                          "  <%s> a nie:InformationElement ; nie:rootElementOf <%s> ; nie:version \"%d\""
                          "}",
                          job->datasource_urn,
                          job->datasource_urn, klass->miner_identifier,
                          job->root_element_urn, job->datasource_urn, klass->version);

  tracker_sparql_connection_update (job->connection,
                                    datasource_insert->str,
                                    G_PRIORITY_DEFAULT,
                                    job->cancellable,
                                    error);

  g_string_free (datasource_insert, TRUE);
}

static void
gd_account_miner_job_query_existing (GdAccountMinerJob *job,
                                     GError **error)
{
  GString *select;
  TrackerSparqlCursor *cursor;

  select = g_string_new (NULL);
  g_string_append_printf (select,
                          "SELECT ?urn nao:identifier(?urn) WHERE { ?urn nie:dataSource <%s> }",
                          job->datasource_urn);

  cursor = tracker_sparql_connection_query (job->connection,
                                            select->str,
                                            job->cancellable,
                                            error);
  g_string_free (select, TRUE);

  if (cursor == NULL)
    return;

  while (tracker_sparql_cursor_next (cursor, job->cancellable, error))
    {
      g_hash_table_insert (job->previous_resources,
                           g_strdup (tracker_sparql_cursor_get_string (cursor, 1, NULL)),
                           g_strdup (tracker_sparql_cursor_get_string (cursor, 0, NULL)));
    }

  g_object_unref (cursor);
}

static void
previous_resources_cleanup_foreach (gpointer key,
                                    gpointer value,
                                    gpointer user_data)
{
  const gchar *resource = value;
  GString *delete = user_data;

  g_string_append_printf (delete, "<%s> a rdfs:Resource . ", resource);
}

static void
gd_account_miner_job_cleanup_previous (GdAccountMinerJob *job,
                                       GError **error)
{
  GString *delete;

  delete = g_string_new (NULL);
  g_string_append (delete, "DELETE { ");

  /* the resources left here are those who were in the database,
   * but were not found during the query; remove them from the database.
   */
  g_hash_table_foreach (job->previous_resources,
                        previous_resources_cleanup_foreach,
                        delete);

  g_string_append (delete, "}");

  tracker_sparql_connection_update (job->connection,
                                    delete->str,
                                    G_PRIORITY_DEFAULT,
                                    job->cancellable,
                                    error);

  g_string_free (delete, TRUE);
}

static void
gd_account_miner_job_query (GdAccountMinerJob *job,
                            GError **error)
{
  GdMinerClass *miner_class = GD_MINER_GET_CLASS (job->miner);

  miner_class->query (job, error);
}

static gboolean
gd_account_miner_job (GIOSchedulerJob *sched_job,
                      GCancellable *cancellable,
                      gpointer user_data)
{
  GdAccountMinerJob *job = user_data;
  GError *error = NULL;

  gd_account_miner_job_ensure_datasource (job, &error);

  if (error != NULL)
    goto out;

  gd_account_miner_job_query_existing (job, &error);

  if (error != NULL)
    goto out;

  gd_account_miner_job_query (job, &error);

  if (error != NULL)
    goto out;

  gd_account_miner_job_cleanup_previous (job, &error);

  if (error != NULL)
    goto out;

 out:
  if (error != NULL)
    g_simple_async_result_take_error (job->async_result, error);

  g_simple_async_result_complete_in_idle (job->async_result);

  return FALSE;
}

static void
gd_account_miner_job_process_async (GdAccountMinerJob *job,
                                    GAsyncReadyCallback callback,
                                    gpointer user_data)
{
  g_assert (job->async_result == NULL);

  job->async_result = g_simple_async_result_new (NULL, callback, user_data,
                                                 gd_account_miner_job_process_async);
  g_simple_async_result_set_op_res_gpointer (job->async_result, job, NULL);

  g_io_scheduler_push_job (gd_account_miner_job, job, NULL,
                           G_PRIORITY_DEFAULT,
                           job->cancellable);
}

static gboolean
gd_account_miner_job_process_finish (GAsyncResult *res,
                                     GError **error)
{
  GSimpleAsyncResult *simple_res = G_SIMPLE_ASYNC_RESULT (res);

  g_assert (g_simple_async_result_is_valid (res, NULL,
                                            gd_account_miner_job_process_async));

  if (g_simple_async_result_propagate_error (simple_res, error))
    return FALSE;

  return TRUE;
}

static void
miner_cancellable_cancelled_cb (GCancellable *cancellable,
                                gpointer user_data)
{
  GdAccountMinerJob *job = user_data;

  /* forward the cancel signal to the ongoing job */
  g_cancellable_cancel (job->cancellable);
}

static GdAccountMinerJob *
gd_account_miner_job_new (GdMiner *self,
                          GoaObject *object)
{
  GdAccountMinerJob *retval;
  GoaAccount *account;
  GdMinerClass *miner_class = GD_MINER_GET_CLASS (self);

  account = goa_object_get_account (object);
  g_assert (account != NULL);

  retval = g_slice_new0 (GdAccountMinerJob);
  retval->miner = g_object_ref (self);
  retval->cancellable = g_cancellable_new ();
  retval->account = account;
  retval->connection = self->priv->connection;
  retval->previous_resources =
    g_hash_table_new_full (g_str_hash, g_str_equal,
                           (GDestroyNotify) g_free, (GDestroyNotify) g_free);

  if (self->priv->cancellable != NULL)
      retval->miner_cancellable_id =
        g_cancellable_connect (self->priv->cancellable,
                               G_CALLBACK (miner_cancellable_cancelled_cb),
                               retval, NULL);

  retval->service = miner_class->create_service (self, object);
  retval->datasource_urn = g_strdup_printf ("gd:goa-account:%s",
                                            goa_account_get_id (retval->account));
  retval->root_element_urn = g_strdup_printf ("gd:goa-account:%s:root-element",
                                              goa_account_get_id (retval->account));

  return retval;
}

static void
miner_job_process_ready_cb (GObject *source,
                            GAsyncResult *res,
                            gpointer user_data)
{
  GdAccountMinerJob *job = user_data;
  GdMiner *self = job->miner;
  GError *error = NULL;

  gd_account_miner_job_process_finish (res, &error);

  if (error != NULL)
    {
      g_printerr ("Error while refreshing account %s: %s",
                  goa_account_get_id (job->account), error->message);

      g_error_free (error);
    }

  self->priv->pending_jobs = g_list_remove (self->priv->pending_jobs,
                                            job);
  gd_account_miner_job_free (job);

  gd_miner_check_pending_jobs (self);
}

static void
gd_miner_setup_account (GdMiner *self,
                        GoaObject *object)
{
  GdAccountMinerJob *job;

  job = gd_account_miner_job_new (self, object);
  self->priv->pending_jobs = g_list_prepend (self->priv->pending_jobs, job);

  gd_account_miner_job_process_async (job, miner_job_process_ready_cb, job);
}

typedef struct {
  GdMiner *self;
  GList *doc_objects;
  GList *acc_objects;
  GList *old_datasources;
} CleanupJob;

static gboolean
cleanup_old_accounts_done (gpointer data)
{
  CleanupJob *job = data;
  GList *l;
  GoaObject *object;
  GdMiner *self = job->self;

  /* now setup all the current accounts */
  for (l = job->doc_objects; l != NULL; l = l->next)
    {
      object = l->data;
      gd_miner_setup_account (self, object);

      g_object_unref (object);
    }

  if (job->doc_objects != NULL)
    {
      g_list_free (job->doc_objects);
      job->doc_objects = NULL;
    }

  if (job->acc_objects != NULL)
    {
      g_list_free_full (job->acc_objects, g_object_unref);
      job->acc_objects = NULL;
    }

  if (job->old_datasources != NULL)
    {
      g_list_free_full (job->old_datasources, g_free);
      job->old_datasources = NULL;
    }

  gd_miner_check_pending_jobs (self);

  g_clear_object (&job->self);
  g_slice_free (CleanupJob, job);

  return FALSE;
}

static void
cleanup_job_do_cleanup (CleanupJob *job)
{
  GdMiner *self = job->self;
  GList *l;
  GString *update;
  GError *error = NULL;

  if (job->old_datasources == NULL)
    return;

  update = g_string_new (NULL);

  for (l = job->old_datasources; l != NULL; l = l->next)
    {
      const gchar *resource;

      resource = l->data;
      g_debug ("Cleaning up old datasource %s", resource);

      g_string_append_printf (update,
                              "DELETE {"
                              "  ?u a rdfs:Resource"
                              "} WHERE {"
                              "  ?u nie:dataSource <%s>"
                              "}",
                              resource);
    }

  tracker_sparql_connection_update (self->priv->connection,
                                    update->str,
                                    G_PRIORITY_DEFAULT,
                                    self->priv->cancellable,
                                    &error);
  g_string_free (update, TRUE);

  if (error != NULL)
    {
      g_printerr ("Error while cleaning up old accounts: %s\n", error->message);
      g_error_free (error);
    }
}

static gint
cleanup_datasource_compare (gconstpointer a,
                            gconstpointer b)
{
  GoaObject *object = GOA_OBJECT (a);
  const gchar *datasource = b;
  gint res;

  GoaAccount *account;
  gchar *object_datasource;

  account = goa_object_peek_account (object);
  g_assert (account != NULL);

  object_datasource = g_strdup_printf ("gd:goa-account:%s", goa_account_get_id (account));
  res = g_strcmp0 (datasource, object_datasource);

  g_free (object_datasource);

  return res;
}

static gboolean
cleanup_job (GIOSchedulerJob *sched_job,
             GCancellable *cancellable,
             gpointer user_data)
{
  GString *select;
  GError *error = NULL;
  TrackerSparqlCursor *cursor;
  const gchar *datasource, *old_version_str;
  gint old_version;
  GList *element;
  CleanupJob *job = user_data;
  GdMiner *self = job->self;
  GdMinerClass *klass = GD_MINER_GET_CLASS (self);

  /* find all our datasources in the tracker DB */
  select = g_string_new (NULL);
  g_string_append_printf (select, "SELECT ?datasource nie:version(?root) WHERE { "
                          "?datasource a nie:DataSource . "
                          "?datasource nao:identifier \"%s\" . "
                          "OPTIONAL { ?root nie:rootElementOf ?datasource } }",
                          klass->miner_identifier);

  cursor = tracker_sparql_connection_query (self->priv->connection,
                                            select->str,
                                            self->priv->cancellable,
                                            &error);
  g_string_free (select, TRUE);

  if (error != NULL)
    {
      g_printerr ("Error while cleaning up old accounts: %s\n", error->message);
      goto out;
    }

  while (tracker_sparql_cursor_next (cursor, self->priv->cancellable, NULL))
    {
      /* If the source we found is not in the current list, add
       * it to the cleanup list.
       * Note that the objects here in the list might *not* support
       * documents, in case the switch has been disabled in System Settings.
       * In fact, we only remove all the account data in case the account
       * is really removed from the panel.
       *
       * Also, cleanup sources for which the version has increased.
       */
      datasource = tracker_sparql_cursor_get_string (cursor, 0, NULL);
      element = g_list_find_custom (job->acc_objects, datasource,
                                    cleanup_datasource_compare);

      if (element == NULL)
        job->old_datasources = g_list_prepend (job->old_datasources,
                                               g_strdup (datasource));

      old_version_str = tracker_sparql_cursor_get_string (cursor, 1, NULL);
      if (old_version_str == NULL)
        old_version = 1;
      else
        sscanf (old_version_str, "%d", &old_version);

      g_debug ("Stored version: %d - new version %d", old_version, klass->version);

      if ((element == NULL) || (old_version < klass->version))
        {
          job->old_datasources = g_list_prepend (job->old_datasources,
                                                 g_strdup (datasource));
        }
    }

  g_object_unref (cursor);

  /* cleanup the DB */
  cleanup_job_do_cleanup (job);

 out:
  g_io_scheduler_job_send_to_mainloop_async (sched_job,
                                             cleanup_old_accounts_done, job, NULL);
  return FALSE;
}

static void
gd_miner_cleanup_old_accounts (GdMiner *self,
                               GList *doc_objects,
                               GList *acc_objects)
{
  CleanupJob *job = g_slice_new0 (CleanupJob);

  job->self = g_object_ref (self);
  job->doc_objects = doc_objects;
  job->acc_objects = acc_objects;

  g_io_scheduler_push_job (cleanup_job, job, NULL,
                           G_PRIORITY_DEFAULT,
                           self->priv->cancellable);
}

static void
gd_miner_refresh_db_real (GdMiner *self)
{
  GoaDocuments *documents;
  GoaPhotos *photos;
  GoaAccount *account;
  GoaObject *object;
  const gchar *provider_type;
  GList *accounts, *doc_objects, *acc_objects, *l;
  GdMinerClass *miner_class = GD_MINER_GET_CLASS (self);

  doc_objects = NULL;
  acc_objects = NULL;

  accounts = goa_client_get_accounts (self->priv->client);
  for (l = accounts; l != NULL; l = l->next)
    {
      object = l->data;

      account = goa_object_peek_account (object);
      if (account == NULL)
        continue;

      provider_type = goa_account_get_provider_type (account);
      if (g_strcmp0 (provider_type, miner_class->goa_provider_type) != 0)
        continue;

      acc_objects = g_list_append (acc_objects, g_object_ref (object));

      documents = goa_object_peek_documents (object);
      photos = goa_object_peek_photos (object);
      if (documents == NULL && photos == NULL)
        continue;

      doc_objects = g_list_append (doc_objects, g_object_ref (object));
    }

  g_list_free_full (accounts, g_object_unref);

  gd_miner_cleanup_old_accounts (self, doc_objects, acc_objects);
}

static void
sparql_connection_ready_cb (GObject *object,
                            GAsyncResult *res,
                            gpointer user_data)
{
  GError *error = NULL;
  GdMiner *self = user_data;

  self->priv->connection = tracker_sparql_connection_get_finish (res, &error);

  if (error != NULL)
    {
      gd_miner_complete_error (self, error);
      return;
    }

  gd_miner_refresh_db_real (self);
}

const gchar *
gd_miner_get_display_name (GdMiner *self)
{
  return self->priv->display_name;
}

void
gd_miner_refresh_db_async (GdMiner *self,
                           GCancellable *cancellable,
                           GAsyncReadyCallback callback,
                           gpointer user_data)
{
  if (self->priv->client_error != NULL)
    {
      gd_miner_complete_error (self, self->priv->client_error);
      return;
    }

  self->priv->result =
    g_simple_async_result_new (G_OBJECT (self),
                               callback, user_data,
                               gd_miner_refresh_db_async);
  self->priv->cancellable =
    (cancellable != NULL) ? g_object_ref (cancellable) : NULL;

  tracker_sparql_connection_get_async (self->priv->cancellable,
                                       sparql_connection_ready_cb, self);
}

gboolean
gd_miner_refresh_db_finish (GdMiner *self,
                            GAsyncResult *res,
                            GError **error)
{
  GSimpleAsyncResult *simple_res = G_SIMPLE_ASYNC_RESULT (res);

  g_assert (g_simple_async_result_is_valid (res, G_OBJECT (self),
                                            gd_miner_refresh_db_async));

  if (g_simple_async_result_propagate_error (simple_res, error))
    return FALSE;

  return TRUE;
}
