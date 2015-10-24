/* GIO - GLib Input, Output and Streaming Library
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

#ifndef __G_KDBUS_H__
#define __G_KDBUS_H__

#if !defined (GIO_COMPILATION)
#error "gkdbus.h is a private header file."
#endif

#include <gio/giotypes.h>
#include "gdbusprivate.h"

#ifdef POLICY_IN_LIB
#include <dbus-1.0/dbus/dbus-policy.h>
#endif

G_BEGIN_DECLS

#define G_TYPE_KDBUS                                       (_g_kdbus_get_type ())
#define G_KDBUS(o)                                         (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_KDBUS, GKdbus))
#define G_KDBUS_CLASS(k)                                   (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_KDBUS, GKdbusClass))
#define G_KDBUS_GET_CLASS(o)                               (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_KDBUS, GKdbusClass))
#define G_IS_KDBUS(o)                                      (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_KDBUS))
#define G_IS_KDBUS_CLASS(k)                                (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_KDBUS))

typedef struct _GKdbus                                      GKdbus;
typedef struct _GKdbusClass                                 GKdbusClass;
typedef struct _GKdbusPrivate                               GKdbusPrivate;

struct _GKdbusClass
{
  GObjectClass parent_class;
};

struct _GKdbus
{
  GObject parent_instance;
  GKdbusPrivate *priv;
};

GType       _g_kdbus_get_type            (void) G_GNUC_CONST;

gboolean    g_kdbus_open                 (GKdbus          *kdbus,
                                          const gchar     *address,
                                          GCancellable    *cancellable,
                                          GError         **error);

gboolean    g_kdbus_close                (GKdbus          *kdbus,
                                          GError         **error);

gboolean    g_kdbus_is_closed            (GKdbus          *kdbus);

GSource    *_g_kdbus_create_source       (GKdbus          *kdbus,
                                          GIOCondition     condition,
                                          GCancellable    *cancellable);

gssize      g_kdbus_send_message         (GDBusWorker     *worker,
                                          GKdbus          *kdbus,
                                          GDBusMessage    *dbus_msg,
                                          gchar           *blob,
                                          gsize            blob_size,
                                          GCancellable    *cancellable,
                                          GError         **error);

gssize      g_kdbus_receive              (GKdbus          *kdbus,
                                          GCancellable    *cancellable,
                                          GError         **error);

gchar      *g_kdbus_get_msg_buffer_ptr   (GKdbus          *kdbus);

void        g_kdbus_release_kmsg         (GKdbus          *kdbus,
                                          gsize            size);

void        g_kdbus_attach_fds_to_msg    (GKdbus          *kdbus,
                                          GUnixFDList    **fd_list);

#ifdef POLICY_IN_LIB
PBusClientPolicy  *_g_kdbus_get_policy       (GKdbus            *kdbus);

gboolean           policy_check_can_receive  (PBusClientPolicy  *policy,
                                              GDBusMessage      *message);

void               send_cant_recv_error      (GDBusConnection   *connection,
                                              GDBusMessage      *dbus_msg,
                                              GKdbus            *kdbus);
#endif

G_END_DECLS

#endif /* __G_KDBUS_H__ */
