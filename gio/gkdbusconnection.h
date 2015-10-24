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

#ifndef __G_KDBUS_CONNECTION_H__
#define __G_KDBUS_CONNECTION_H__

#if !defined (__GIO_GIO_H_INSIDE__) && !defined (GIO_COMPILATION)
#error "Only <gio/gio.h> can be included directly."
#endif

#include <glib-object.h>
#include <gio/gkdbus.h>
#include <gio/giostream.h>

G_BEGIN_DECLS

#define G_TYPE_KDBUS_CONNECTION                            (g_kdbus_connection_get_type ())
#define G_KDBUS_CONNECTION(inst)                           (G_TYPE_CHECK_INSTANCE_CAST ((inst),                     \
                                                             G_TYPE_KDBUS_CONNECTION, GKdbusConnection))
#define G_KDBUS_CONNECTION_CLASS(class)                    (G_TYPE_CHECK_CLASS_CAST ((class),                       \
                                                             G_TYPE_KDBUS_CONNECTION, GKdbusConnectionClass))
#define G_IS_KDBUS_CONNECTION(inst)                        (G_TYPE_CHECK_INSTANCE_TYPE ((inst),                     \
                                                             G_TYPE_KDBUS_CONNECTION))
#define G_IS_KDBUS_CONNECTION_CLASS(class)                 (G_TYPE_CHECK_CLASS_TYPE ((class),                       \
                                                             G_TYPE_KDBUS_CONNECTION))
#define G_KDBUS_CONNECTION_GET_CLASS(inst)                 (G_TYPE_INSTANCE_GET_CLASS ((inst),                      \
                                                             G_TYPE_KDBUS_CONNECTION, GKdbusConnectionClass))

typedef struct _GKdbusConnectionPrivate                    GKdbusConnectionPrivate;
typedef struct _GKdbusConnectionClass                      GKdbusConnectionClass;

struct _GKdbusConnectionClass
{
  GIOStreamClass parent_class;

  gboolean (* close_fn)      (GKdbusConnection    *connection,
                              GCancellable        *cancellable,
                              GError             **error);
  void (* close_async)       (GKdbusConnection    *connection,
                              int                  io_priority,
                              GCancellable        *cancellable,
                              GAsyncReadyCallback  callback,
                              gpointer             user_data);
  gboolean (* close_finish)  (GKdbusConnection    *connection,
                              GAsyncResult        *result,
                              GError             **error);

  /* Padding for future expansion */
  void (*_g_reserved1) (void);
  void (*_g_reserved2) (void);
  void (*_g_reserved3) (void);
  void (*_g_reserved4) (void);
  void (*_g_reserved5) (void);
  void (*_g_reserved6) (void);
};

struct _GKdbusConnection
{
  GIOStream parent_instance;
  GKdbusConnectionPrivate *priv;
  GDBusCapabilityFlags capabilities;
};


GType              g_kdbus_connection_get_type        (void) G_GNUC_CONST;

GKdbusConnection  *g_kdbus_connection_new             (void);

gboolean           g_kdbus_connection_is_connected    (GKdbusConnection     *connection);

gboolean           g_kdbus_connection_connect         (GKdbusConnection     *connection,
                                                       const gchar          *address,
                                                       GCancellable         *cancellable,
                                                       GError              **error);

gboolean           g_kdbus_connection_close           (GKdbusConnection     *connection,
                                                       GCancellable         *cancellable,
                                                       GError              **error);

GKdbus            *g_kdbus_connection_get_kdbus       (GKdbusConnection     *connection);

void               g_kdbus_connection_close_async     (GIOStream            *stream,
                                                       int                   io_priority,
                                                       GCancellable         *cancellable,
                                                       GAsyncReadyCallback   callback,
                                                       gpointer              user_data);

gboolean           g_kdbus_connection_close_finish    (GIOStream            *stream,
                                                       GAsyncResult         *result,
                                                       GError              **error);

G_END_DECLS

#endif /* __G_KDBUS_CONNECTION_H__ */