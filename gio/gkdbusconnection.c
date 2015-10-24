/*  GIO - GLib Input, Output and Streaming Library
 *
 * Copyright © 2013 Samsung Electronics
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Michal Eljasiewicz   <m.eljasiewic@samsung.com>
 * Author: Lukasz Skalski       <l.skalski@partner.samsung.com>
 */

#include <fcntl.h>
#include "config.h"

#include "gkdbusconnection.h"
#include "gunixconnection.h"

#include "glibintl.h"

/**
 * SECTION:gkdbusconnection
 * @short_description: A kdbus connection
 * @include: gio/gio.h
 * @see_also: #GIOStream, #GKdbusClient
 *
 * #GKdbusConnection is a #GIOStream for a connected kdbus bus.
 */

enum
{
  PROP_0,
  PROP_CLOSED
};

G_DEFINE_TYPE (GKdbusConnection, g_kdbus_connection, G_TYPE_IO_STREAM);

struct _GKdbusConnectionPrivate
{
  GKdbus        *kdbus;
  gboolean       in_dispose;
  guint          closed;

  GAsyncReadyCallback outstanding_callback;
};

/*
 * g_kdbus_connection_new:
 *
 */
GKdbusConnection *
g_kdbus_connection_new (void)
{
  return g_object_new(G_TYPE_KDBUS_CONNECTION,NULL);
}

/*
 * g_kdbus_connection_connect:
 *
 */
gboolean
g_kdbus_connection_connect  (GKdbusConnection   *connection,
                             const gchar        *address,
                             GCancellable       *cancellable,
                             GError             **error)
{
  g_return_val_if_fail (G_IS_KDBUS_CONNECTION (connection), FALSE);

  return g_kdbus_open (connection->priv->kdbus,address,cancellable,error);
}

/*
 * g_kdbus_connection_is_connected:
 *
 */
gboolean
g_kdbus_connection_is_connected (GKdbusConnection  *connection)
{
  return (!g_kdbus_is_closed (connection->priv->kdbus));
}

/*
 * g_kdbus_connection_get_property:
 *
 */
static void
g_kdbus_connection_get_property (GObject        *object,
                                 guint           prop_id,
                                 GValue         *value,
                                 GParamSpec     *pspec)
{
  //GKdbusConnection *connection = G_KDBUS_CONNECTION (object);
  switch (prop_id)
    {
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/*
 * g_kdbus_connection_set_property
 *
 */
static void
g_kdbus_connection_set_property (GObject        *object,
                                 guint           prop_id,
                                 const GValue    *value,
                                 GParamSpec      *pspec)
{
  //GKdbusConnection *connection = G_KDBUS_CONNECTION (object);
  switch (prop_id)
    {
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

// TODO
static void
g_kdbus_connection_dispose (GObject *object)
{
  GKdbusConnection *connection = G_KDBUS_CONNECTION (object);

  if (!connection->priv->closed)
    g_kdbus_connection_close (connection, NULL, NULL);
}

/*
 * g_kdbus_connection_finalize:
 *
 */
static void
g_kdbus_connection_finalize (GObject *object)
{
  GKdbusConnection *connection;

  //GKdbusConnection *connection = G_KDBUS_CONNECTION (object);
  G_OBJECT_CLASS (g_kdbus_connection_parent_class)
    ->finalize (object);

  connection = G_KDBUS_CONNECTION (object);
  g_object_unref(connection->priv->kdbus);
}

static gboolean
g_kdbus_connection_real_close (GKdbusConnection *connection,
                               GCancellable     *cancellable,
                               GError          **error)
{
  gboolean res;

  res = g_kdbus_close (connection->priv->kdbus, NULL);

  return res;
}

/*
 * g_kdbus_connection_close
 *
 */
gboolean
g_kdbus_connection_close (GKdbusConnection      *connection,
                          GCancellable          *cancellable,
                          GError                **error)
{
  GKdbusConnectionClass *class;
  gboolean res;

  g_return_val_if_fail (G_IS_KDBUS_CONNECTION (connection), FALSE);

  class = G_KDBUS_CONNECTION_GET_CLASS (connection);

  if (connection->priv->in_dispose)
    return TRUE;

  if (connection->priv->closed)
    return TRUE;

  if (cancellable)
    g_cancellable_push_current (cancellable);

  res = TRUE;
  if (class->close_fn)
    res = class->close_fn (connection, cancellable, error);

  if (cancellable)
    g_cancellable_pop_current (cancellable);

  connection->priv->closed = TRUE;

  return res;
}

static void
close_async_thread (GSimpleAsyncResult      *res,
                    GObject                 *object,
                    GCancellable            *cancellable)
{
  GKdbusConnection *connection = G_KDBUS_CONNECTION (object);
  GKdbusConnectionClass *class = G_KDBUS_CONNECTION_GET_CLASS (object);
  GError *error = NULL;
  gboolean result;

  if (class->close_fn)
    {
      result = class->close_fn (connection, cancellable, NULL /* error */);
      if (!result)
        g_simple_async_result_take_error (res, error);
    }
}

static void
g_kdbus_connection_real_close_async (GKdbusConnection    *connection,
                                     int                  io_priority,
                                     GCancellable        *cancellable,
                                     GAsyncReadyCallback  callback,
                                     gpointer             user_data)
{
  GSimpleAsyncResult *res;

  res = g_simple_async_result_new (G_OBJECT (connection),
                                   callback,
                                   user_data,
                                   g_kdbus_connection_real_close_async);

  g_simple_async_result_set_handle_cancellation (res, FALSE);

  g_simple_async_result_run_in_thread (res,
                                       close_async_thread,
                                       io_priority,
                                       cancellable);
  g_object_unref (res);
}

static void
async_ready_close_callback_wrapper (GObject         *source_object,
                                    GAsyncResult    *res,
                                    gpointer         user_data)
{
  GKdbusConnection *connection = G_KDBUS_CONNECTION (source_object);

  connection->priv->closed = TRUE;
  if (connection->priv->outstanding_callback)
    (*connection->priv->outstanding_callback) (source_object, res, user_data);
  g_object_unref (connection);
}

void
g_kdbus_connection_close_async (GIOStream           *stream,
                                int                  io_priority,
                                GCancellable        *cancellable,
                                GAsyncReadyCallback  callback,
                                gpointer             user_data)
{
  GKdbusConnection *connection = G_KDBUS_CONNECTION (stream);
  GKdbusConnectionClass *class = G_KDBUS_CONNECTION_GET_CLASS (connection);
  GSimpleAsyncResult *simple;

  if (connection->priv->closed)
    {
      simple = g_simple_async_result_new (G_OBJECT (connection),
                                          callback,
                                          user_data,
                                          g_kdbus_connection_close_async);
      g_simple_async_result_complete_in_idle (simple);
      g_object_unref (simple);
      return;
    }

  connection->priv->outstanding_callback = callback;
  g_object_ref (connection);
  class->close_async (connection, io_priority, cancellable,
                      async_ready_close_callback_wrapper, user_data);
}

static gboolean
g_kdbus_connection_real_close_finish (GKdbusConnection  *connection,
                                      GAsyncResult      *result,
                                      GError           **error)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (result);
  g_warn_if_fail (g_simple_async_result_get_source_tag (simple) ==
                               g_kdbus_connection_real_close_async);
  return TRUE;
}

gboolean
g_kdbus_connection_close_finish (GIOStream      *stream,
                                 GAsyncResult   *result,
                                 GError        **error)
{
  GSimpleAsyncResult *simple;
  GKdbusConnection *connection;
  GKdbusConnectionClass *class;

  g_return_val_if_fail (G_IS_KDBUS_CONNECTION (stream), FALSE);
  g_return_val_if_fail (G_IS_ASYNC_RESULT (result), FALSE);

  if (G_IS_SIMPLE_ASYNC_RESULT (result))
    {
      simple = G_SIMPLE_ASYNC_RESULT (result);
      if (g_simple_async_result_propagate_error (simple, error))
        return FALSE;

      /* Special case already closed */
      if (g_simple_async_result_get_source_tag (simple) == g_kdbus_connection_close_async)
        return TRUE;
    }

  connection = G_KDBUS_CONNECTION (stream);
  class = G_KDBUS_CONNECTION_GET_CLASS (stream);
  return class->close_finish (connection, result, error);
}

/*
 * g_kdbus_connection_class_init:
 *
 */
static void
g_kdbus_connection_class_init (GKdbusConnectionClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (GKdbusConnectionPrivate));

  gobject_class->set_property = g_kdbus_connection_set_property;
  gobject_class->get_property = g_kdbus_connection_get_property;
  gobject_class->finalize = g_kdbus_connection_finalize;
  gobject_class->dispose = g_kdbus_connection_dispose;

  klass->close_fn = g_kdbus_connection_real_close;
  klass->close_async = g_kdbus_connection_real_close_async;
  klass->close_finish = g_kdbus_connection_real_close_finish;

  g_object_class_install_property (gobject_class, PROP_CLOSED,
                                   g_param_spec_boolean ("closed",
                                                         P_("Closed"),
                                                         P_("Is the connection closed"),
                                                         FALSE,
                                                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
}

/*
 * g_kdbus_connection_init:
 *
 */
static void
g_kdbus_connection_init (GKdbusConnection *connection)
{
  connection->priv = G_TYPE_INSTANCE_GET_PRIVATE (connection,
                                                  G_TYPE_KDBUS_CONNECTION,
                                                  GKdbusConnectionPrivate);
  connection->priv->kdbus = g_object_new(G_TYPE_KDBUS,NULL);
}

/*
 * g_kdbus_connection_get_kdbus: gets the underlying #GKdbus object of the connection.
 *
 */
GKdbus *
g_kdbus_connection_get_kdbus (GKdbusConnection *connection)
{
  g_return_val_if_fail (G_IS_KDBUS_CONNECTION (connection), NULL);

  return connection->priv->kdbus;
}

