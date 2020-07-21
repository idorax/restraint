/*
  This file is part of Restraint.

  Restraint is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Restraint is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Restraint.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "logging.h"
#include "task.h"

#ifndef LOG_MANAGER_DIR
#define LOG_MANAGER_DIR VAR_LIB_PATH "/logs"
#endif

struct _RstrntLogManager
{
    GObject parent_instance;

    GHashTable *logs;
};

G_DEFINE_TYPE (RstrntLogManager, rstrnt_log_manager, G_TYPE_OBJECT)

typedef struct
{
    GFile *file;
    GFileOutputStream *output_stream;
} RstrntLogData;

typedef struct
{
    RstrntLogData *log_data;
    GVariant *variant;
} RstrntLogWriterData;

typedef struct
{
    RstrntLogData *task_log_data;
    RstrntLogData *harness_log_data;

    GThreadPool *thread_pool;
} RstrntTaskLogData;

static void
rstrnt_log_manager_dispose (GObject *object)
{
    RstrntLogManager *self;

    self = RSTRNT_LOG_MANAGER (object);

    if (NULL != self->logs)
    {
        g_hash_table_destroy (self->logs);
        self->logs = NULL;
    }

    G_OBJECT_CLASS (rstrnt_log_manager_parent_class)->dispose (object);
}

static void
rstrnt_log_manager_class_init (RstrntLogManagerClass *klass)
{
    GObjectClass *object_class;

    object_class = G_OBJECT_CLASS (klass);

    object_class->dispose = rstrnt_log_manager_dispose;
}

static void
rstrnt_log_data_destroy (gpointer data)
{
    RstrntLogData *log_data;

    log_data = data;

    g_clear_object (&log_data->output_stream);
    g_clear_object (&log_data->file);

    g_free (log_data);
}

static void
rstrnt_task_log_data_destroy (gpointer data)
{
    RstrntTaskLogData *task_log_data;

    task_log_data = data;

    rstrnt_log_data_destroy (task_log_data->task_log_data);
    rstrnt_log_data_destroy (task_log_data->harness_log_data);
    g_thread_pool_free (task_log_data->thread_pool, TRUE, TRUE);

    g_free (task_log_data);
}

static void
rstrnt_log_manager_init (RstrntLogManager *self)
{
    self->logs = g_hash_table_new_full (g_str_hash, g_str_equal,
                                        NULL, rstrnt_task_log_data_destroy);
}

static RstrntLogData *
rstrnt_log_data_new (GFile   *file,
                     GError **error)
{
    RstrntLogData *data;

    data = g_new0 (RstrntLogData, 1);

    data->file = g_object_ref (file);
    data->output_stream = g_file_append_to (data->file, G_FILE_CREATE_NONE,
                                            NULL, error);
    if (NULL == data->output_stream)
    {
        rstrnt_log_data_destroy (data);

        return NULL;
    }

    return data;
}

static void
rstrnt_write_log_func (gpointer data,
                       gpointer user_data)
{
    g_autofree RstrntLogWriterData *writer_data = NULL;
    g_autoptr (GVariant) variant = NULL;
    const char *message;
    gsize message_length;
    GError *error = NULL;
    bool success;

    writer_data = data;
    variant = writer_data->variant;

    /* This is a sentinel */
    if (variant == NULL && writer_data->log_data == NULL) {
        g_debug ("%s(): Got data sentinel", __func__);

        return;
    }

    message = g_variant_get_fixed_array (variant, &message_length, sizeof (*message));
    success = g_output_stream_write_all (G_OUTPUT_STREAM (writer_data->log_data->output_stream),
                                         message, message_length,
                                         NULL, NULL, &error);

    if (!success)
    {
        g_warning ("%s(): Failed to write out log message: %s",
                   __func__, error->message);
    }
}

static RstrntTaskLogData *
rstrnt_task_log_data_new (const RstrntTask  *task,
                          GError           **error)
{
    g_autofree char *log_directory_path = NULL;
    g_autoptr (GFile) log_directory = NULL;
    g_autoptr (GFile) task_log_file = NULL;
    g_autoptr (GFileOutputStream) task_log_file_output_stream = NULL;
    g_autoptr (GFile) harness_log_file = NULL;
    g_autoptr (GFileOutputStream) harness_log_file_output_stream = NULL;
    RstrntTaskLogData *data;

    log_directory_path = g_build_path ("/", LOG_MANAGER_DIR, task->task_id, NULL);
    log_directory = g_file_new_for_path (log_directory_path);

    if (!g_file_make_directory_with_parents (log_directory, NULL, error))
    {
        if (NULL != error && G_IO_ERROR_EXISTS != (*error)->code)
        {
            return NULL;
        }

        g_clear_error (error);
    }

    task_log_file = g_file_get_child (log_directory, "task.log");
    harness_log_file = g_file_get_child (log_directory, "harness.log");
    data = g_new0 (RstrntTaskLogData, 1);

    data->task_log_data = rstrnt_log_data_new (task_log_file, error);
    if (NULL == data->task_log_data)
    {
        rstrnt_task_log_data_destroy (data);

        return NULL;
    }
    data->harness_log_data = rstrnt_log_data_new (harness_log_file, error);
    if (NULL == data->harness_log_data)
    {
        rstrnt_task_log_data_destroy (data);

        return NULL;
    }
    data->thread_pool = g_thread_pool_new (rstrnt_write_log_func, NULL, 1,
                                           FALSE, error);
    if (NULL == data->thread_pool)
    {
        rstrnt_task_log_data_destroy (data);

        return NULL;
    }

    return data;
}

static RstrntTaskLogData *
rstrnt_log_manager_get_task_data (RstrntLogManager  *self,
                                  const RstrntTask  *task,
                                  GError           **error)
{
    RstrntTaskLogData *data;

    data = g_hash_table_lookup (self->logs, task->task_id);
    if (NULL == data)
    {
        data = rstrnt_task_log_data_new (task, error);
        if (NULL != data)
        {
            (void) g_hash_table_insert (self->logs, task->task_id, data);
        }
    }

    return data;
}

static void
rstrnt_on_log_uploaded (SoupSession *session,
                        SoupMessage *msg,
                        gpointer     user_data)
{
    g_debug ("%s(): response code: %u", __func__, msg->status_code);

    g_mapped_file_unref (user_data);
}

static void
rstrnt_flush_logs (const RstrntTask *task,
                   GCancellable     *cancellable)
{
    RstrntLogManager *manager;
    RstrntTaskLogData *data;
    GOutputStream *stream;
    g_autoptr (GError) error = NULL;
    RstrntLogWriterData *sentinel;

    g_return_if_fail (NULL != task);

    manager = rstrnt_log_manager_get_instance ();
    data = rstrnt_log_manager_get_task_data (manager, task, &error);

    g_return_if_fail (NULL != data);

    /* Sentinel to make sure all logged data is done. Prevents the
       race condition where the last thread with data finishes after
       flushing the stream. */
    sentinel = g_new0 (RstrntLogWriterData, 1);
    (void) g_thread_pool_push (data->thread_pool, sentinel, NULL);

    while (g_thread_pool_unprocessed (data->thread_pool) > 0 &&
           g_thread_pool_get_num_threads (data->thread_pool) > 0)
    {
        g_usleep (G_USEC_PER_SEC / 4);
    }

    stream = G_OUTPUT_STREAM (data->task_log_data->output_stream);

    if (!g_output_stream_flush (stream, cancellable, &error))
    {
        if (G_IO_ERROR_CANCELLED != error->code)
        {
            g_warning ("%s(): Failed to flush task log stream: %s",
                       __func__, error->message);
        }
    }

    stream = G_OUTPUT_STREAM (data->harness_log_data->output_stream);

    if (!g_output_stream_flush (stream, cancellable, &error))
    {
        if (G_IO_ERROR_CANCELLED != error->code)
        {
            g_warning ("%s(): Failed to flush harness log stream: %s",
                       __func__, error->message);
        }
    }
}

static void
rstrnt_upload_log (const RstrntTask    *task,
                   RstrntServerAppData *app_data,
                   SoupSession         *session,
                   GCancellable        *cancellable,
                   RstrntLogType        type)
{
    RstrntLogManager *manager;
    g_autoptr (GError) error = NULL;
    RstrntTaskLogData *data;
    g_autofree char *path = NULL;
    GMappedFile *file;
    SoupURI *uri = NULL;
    SoupMessage *message;
    const char *contents;
    size_t length;
    GFile *data_file = NULL;
    gchar *log_path = NULL;

    manager = rstrnt_log_manager_get_instance ();
    data = rstrnt_log_manager_get_task_data (manager, task, &error);

    g_return_if_fail (NULL != data);

    switch (type)
    {
        case RSTRNT_LOG_TYPE_TASK:
        {
            data_file = data->task_log_data->file;
            log_path = LOG_PATH_TASK;
        }
        break;

        case RSTRNT_LOG_TYPE_HARNESS:
        {
            data_file = data->harness_log_data->file;
            log_path = LOG_PATH_HARNESS;
        }
        break;
    }

    path = g_file_get_path (data_file);
    file = g_mapped_file_new (path, false, &error);

    if (NULL == file)
    {
        g_warning ("Task log file mapping failed: %s", error->message);

        g_return_if_reached ();
    }


    uri = soup_uri_new_with_base (task->task_uri, log_path);
    message = soup_message_new_from_uri ("PUT", uri);
    contents = g_mapped_file_get_contents (file);
    length = g_mapped_file_get_length (file);

    soup_message_headers_append (message->request_headers, "log-level", "2");
    soup_message_set_request (message, "text/plain", SOUP_MEMORY_TEMPORARY,
                              contents, length);

    app_data->queue_message (session,
                             message,
                             NULL,
                             rstrnt_on_log_uploaded,
                             cancellable,
                             file);

    soup_uri_free (uri);
}

void
rstrnt_upload_logs (const RstrntTask    *task,
                    RstrntServerAppData *app_data,
                    SoupSession         *session,
                    GCancellable        *cancellable)
{
    g_autoptr (GTask) flush_task = NULL;

    g_return_if_fail (NULL != task);
    g_return_if_fail (NULL != app_data);
    g_return_if_fail (SOUP_IS_SESSION (session));

    rstrnt_flush_logs (task, cancellable);

    rstrnt_upload_log (task, app_data, session, cancellable, RSTRNT_LOG_TYPE_TASK);
    rstrnt_upload_log (task, app_data, session, cancellable, RSTRNT_LOG_TYPE_HARNESS);
}

static void
rstrnt_log_manager_append_to_log (RstrntLogManager    *self,
                                  const RstrntTask    *task,
                                  RstrntLogType        type,
                                  const char          *message,
                                  size_t               message_length)
{
    g_autoptr (GError) error = NULL;
    RstrntTaskLogData *data;
    RstrntLogWriterData *writer_data;

    data = rstrnt_log_manager_get_task_data (self, task, &error);
    if (NULL == data)
    {
        g_return_if_reached ();
    }
    writer_data = g_new0 (RstrntLogWriterData, 1);

    switch (type)
    {
        case RSTRNT_LOG_TYPE_TASK:
            writer_data->log_data = data->task_log_data;

            break;

        case RSTRNT_LOG_TYPE_HARNESS:
            writer_data->log_data = data->harness_log_data;

            break;
    }

    writer_data->variant = g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE,
                                                      message, message_length,
                                                      sizeof(*message));

    (void) g_thread_pool_push (data->thread_pool, writer_data, NULL);
}

void
rstrnt_log_bytes (const RstrntTask *task,
                  RstrntLogType     type,
                  const char       *message,
                  size_t            message_length)
{
    RstrntLogManager *manager;

    g_return_if_fail (NULL != task);
    g_return_if_fail (NULL != message);

    manager = rstrnt_log_manager_get_instance ();

    rstrnt_log_manager_append_to_log (manager, task, type,
                                      message, message_length);
}

void
rstrnt_log (const RstrntTask *task,
            RstrntLogType     type,
            const char       *format,
            ...)
{
    va_list args;
    g_autofree char *message = NULL;
    size_t message_length;
    RstrntLogManager *manager;

    g_return_if_fail (NULL != task);
    g_return_if_fail (NULL != format);

    va_start (args, format);

    message = g_strdup_vprintf (format, args);

    va_end (args);

    message_length = strlen (message);
    manager = rstrnt_log_manager_get_instance ();

    rstrnt_log_manager_append_to_log (manager, task, type,
                                      message, message_length);
}

static gpointer
rstrnt_log_manager_create_instance (gpointer data)
{
    static RstrntLogManager *instance;

    (void) data;

    instance = g_object_new (RSTRNT_TYPE_LOG_MANAGER, NULL);

    return instance;
}

RstrntLogManager *
rstrnt_log_manager_get_instance (void)
{
    static GOnce once = G_ONCE_INIT;

    g_once (&once, rstrnt_log_manager_create_instance, NULL);

    return once.retval;
}