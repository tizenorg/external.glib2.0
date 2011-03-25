/* GIO - GLib Input, Output and Streaming Library
 *
 * Copyright © 2009 Codethink Limited
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2 of the licence or (at
 * your option) any later version.
 *
 * See the included COPYING file for more information.
 *
 * Authors: Ryan Lortie <desrt@desrt.ca>
 */

#include "config.h"
#include "gunixconnection.h"
#include "gunixcredentialsmessage.h"
#include "glibintl.h"

/**
 * SECTION: gunixconnection
 * @title: GUnixConnection
 * @short_description: A UNIX domain GSocketConnection
 * @include: gio/gunixconnection.h
 * @see_also: #GSocketConnection.
 *
 * This is the subclass of #GSocketConnection that is created
 * for UNIX domain sockets.
 *
 * It contains functions to do some of the UNIX socket specific
 * functionality like passing file descriptors.
 *
 * Note that <filename>&lt;gio/gunixconnection.h&gt;</filename> belongs to
 * the UNIX-specific GIO interfaces, thus you have to use the
 * <filename>gio-unix-2.0.pc</filename> pkg-config file when using it.
 *
 * Since: 2.22
 */

#include <gio/gsocketcontrolmessage.h>
#include <gio/gunixfdmessage.h>
#include <gio/gsocket.h>
#include <unistd.h>

#ifdef __linux__
/* for getsockopt() and setsockopt() */
#include <sys/types.h>          /* See NOTES */
#include <sys/socket.h>
#include <errno.h>
#include <string.h>
#endif


G_DEFINE_TYPE_WITH_CODE (GUnixConnection, g_unix_connection,
			 G_TYPE_SOCKET_CONNECTION,
  g_socket_connection_factory_register_type (g_define_type_id,
					     G_SOCKET_FAMILY_UNIX,
					     G_SOCKET_TYPE_STREAM,
					     G_SOCKET_PROTOCOL_DEFAULT);
			 );

/**
 * g_unix_connection_send_fd:
 * @connection: a #GUnixConnection
 * @fd: a file descriptor
 * @cancellable: (allow-none): optional #GCancellable object, %NULL to ignore.
 * @error: (allow-none): #GError for error reporting, or %NULL to ignore.
 *
 * Passes a file descriptor to the recieving side of the
 * connection. The recieving end has to call g_unix_connection_receive_fd()
 * to accept the file descriptor.
 *
 * As well as sending the fd this also writes a single byte to the
 * stream, as this is required for fd passing to work on some
 * implementations.
 *
 * Returns: a %TRUE on success, %NULL on error.
 *
 * Since: 2.22
 */
gboolean
g_unix_connection_send_fd (GUnixConnection  *connection,
                           gint              fd,
                           GCancellable     *cancellable,
                           GError          **error)
{
  GSocketControlMessage *scm;
  GSocket *socket;

  g_return_val_if_fail (G_IS_UNIX_CONNECTION (connection), FALSE);
  g_return_val_if_fail (fd >= 0, FALSE);

  scm = g_unix_fd_message_new ();

  if (!g_unix_fd_message_append_fd (G_UNIX_FD_MESSAGE (scm), fd, error))
    {
      g_object_unref (scm);
      return FALSE;
    }

  g_object_get (connection, "socket", &socket, NULL);
  if (g_socket_send_message (socket, NULL, NULL, 0, &scm, 1, 0, cancellable, error) != 1)
    /* XXX could it 'fail' with zero? */
    {
      g_object_unref (socket);
      g_object_unref (scm);

      return FALSE;
    }

  g_object_unref (socket);
  g_object_unref (scm);

  return TRUE;
}

/**
 * g_unix_connection_receive_fd:
 * @connection: a #GUnixConnection
 * @cancellable: (allow-none): optional #GCancellable object, %NULL to ignore
 * @error: (allow-none): #GError for error reporting, or %NULL to ignore
 *
 * Receives a file descriptor from the sending end of the connection.
 * The sending end has to call g_unix_connection_send_fd() for this
 * to work.
 *
 * As well as reading the fd this also reads a single byte from the
 * stream, as this is required for fd passing to work on some
 * implementations.
 *
 * Returns: a file descriptor on success, -1 on error.
 *
 * Since: 2.22
 **/
gint
g_unix_connection_receive_fd (GUnixConnection  *connection,
                              GCancellable     *cancellable,
                              GError          **error)
{
  GSocketControlMessage **scms;
  gint *fds, nfd, fd, nscm;
  GUnixFDMessage *fdmsg;
  GSocket *socket;

  g_return_val_if_fail (G_IS_UNIX_CONNECTION (connection), -1);

  g_object_get (connection, "socket", &socket, NULL);
  if (g_socket_receive_message (socket, NULL, NULL, 0,
                                &scms, &nscm, NULL, cancellable, error) != 1)
    /* XXX it _could_ 'fail' with zero. */
    {
      g_object_unref (socket);

      return -1;
    }

  g_object_unref (socket);

  if (nscm != 1)
    {
      gint i;

      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		   _("Expecting 1 control message, got %d"), nscm);

      for (i = 0; i < nscm; i++)
        g_object_unref (scms[i]);

      g_free (scms);

      return -1;
    }

  if (!G_IS_UNIX_FD_MESSAGE (scms[0]))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
			   _("Unexpected type of ancillary data"));
      g_object_unref (scms[0]);
      g_free (scms);

      return -1;
    }

  fdmsg = G_UNIX_FD_MESSAGE (scms[0]);
  g_free (scms);

  fds = g_unix_fd_message_steal_fds (fdmsg, &nfd);
  g_object_unref (fdmsg);

  if (nfd != 1)
    {
      gint i;

      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		   _("Expecting one fd, but got %d\n"), nfd);

      for (i = 0; i < nfd; i++)
        close (fds[i]);

      g_free (fds);

      return -1;
    }

  fd = *fds;
  g_free (fds);

  if (fd < 0)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           _("Received invalid fd"));
      fd = -1;
    }

  return fd;
}

static void
g_unix_connection_init (GUnixConnection *connection)
{
}

static void
g_unix_connection_class_init (GUnixConnectionClass *class)
{
}

/* TODO: Other stuff we might want to add are:
void                    g_unix_connection_send_fd_async                 (GUnixConnection      *connection,
                                                                         gint                  fd,
                                                                         gboolean              close,
                                                                         gint                  io_priority,
                                                                         GAsyncReadyCallback   callback,
                                                                         gpointer              user_data);
gboolean                g_unix_connection_send_fd_finish                (GUnixConnection      *connection,
                                                                         GError              **error);

gboolean                g_unix_connection_send_fds                      (GUnixConnection      *connection,
                                                                         gint                 *fds,
                                                                         gint                  nfds,
                                                                         GError              **error);
void                    g_unix_connection_send_fds_async                (GUnixConnection      *connection,
                                                                         gint                 *fds,
                                                                         gint                  nfds,
                                                                         gint                  io_priority,
                                                                         GAsyncReadyCallback   callback,
                                                                         gpointer              user_data);
gboolean                g_unix_connection_send_fds_finish               (GUnixConnection      *connection,
                                                                         GError              **error);

void                    g_unix_connection_receive_fd_async              (GUnixConnection      *connection,
                                                                         gint                  io_priority,
                                                                         GAsyncReadyCallback   callback,
                                                                         gpointer              user_data);
gint                    g_unix_connection_receive_fd_finish             (GUnixConnection      *connection,
                                                                         GError              **error);


gboolean                g_unix_connection_send_credentials              (GUnixConnection      *connection,
                                                                         GError              **error);
void                    g_unix_connection_send_credentials_async        (GUnixConnection      *connection,
                                                                         gint                  io_priority,
                                                                         GAsyncReadyCallback   callback,
                                                                         gpointer              user_data);
gboolean                g_unix_connection_send_credentials_finish       (GUnixConnection      *connection,
                                                                         GError              **error);

gboolean                g_unix_connection_send_fake_credentials         (GUnixConnection      *connection,
                                                                         guint64               pid,
                                                                         guint64               uid,
                                                                         guint64               gid,
                                                                         GError              **error);
void                    g_unix_connection_send_fake_credentials_async   (GUnixConnection      *connection,
                                                                         guint64               pid,
                                                                         guint64               uid,
                                                                         guint64               gid,
                                                                         gint                  io_priority,
                                                                         GAsyncReadyCallback   callback,
                                                                         gpointer              user_data);
gboolean                g_unix_connection_send_fake_credentials_finish  (GUnixConnection      *connection,
                                                                         GError              **error);

gboolean                g_unix_connection_receive_credentials           (GUnixConnection      *connection,
                                                                         guint64              *pid,
                                                                         guint64              *uid,
                                                                         guint64              *gid,
                                                                         GError              **error);
void                    g_unix_connection_receive_credentials_async     (GUnixConnection      *connection,
                                                                         gint                  io_priority,
                                                                         GAsyncReadyCallback   callback,
                                                                         gpointer              user_data);
gboolean                g_unix_connection_receive_credentials_finish    (GUnixConnection      *connection,
                                                                         guint64              *pid,
                                                                         guint64              *uid,
                                                                         guint64              *gid,
                                                                         GError              **error);

gboolean                g_unix_connection_create_pair                   (GUnixConnection     **one,
                                                                         GUnixConnection     **two,
                                                                         GError              **error);
*/


/**
 * g_unix_connection_send_credentials:
 * @connection: A #GUnixConnection.
 * @cancellable: A #GCancellable or %NULL.
 * @error: Return location for error or %NULL.
 *
 * Passes the credentials of the current user the receiving side
 * of the connection. The recieving end has to call
 * g_unix_connection_receive_credentials() (or similar) to accept the
 * credentials.
 *
 * As well as sending the credentials this also writes a single NUL
 * byte to the stream, as this is required for credentials passing to
 * work on some implementations.
 *
 * Other ways to exchange credentials with a foreign peer includes the
 * #GUnixCredentialsMessage type and g_socket_get_credentials() function.
 *
 * Returns: %TRUE on success, %FALSE if @error is set.
 *
 * Since: 2.26
 */
gboolean
g_unix_connection_send_credentials (GUnixConnection      *connection,
                                    GCancellable         *cancellable,
                                    GError              **error)
{
  GCredentials *credentials;
  GSocketControlMessage *scm;
  GSocket *socket;
  gboolean ret;
  GOutputVector vector;
  guchar nul_byte[1] = {'\0'};

  g_return_val_if_fail (G_IS_UNIX_CONNECTION (connection), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  ret = FALSE;

  credentials = g_credentials_new ();

  vector.buffer = &nul_byte;
  vector.size = 1;
  scm = g_unix_credentials_message_new_with_credentials (credentials);
  g_object_get (connection, "socket", &socket, NULL);
  if (g_socket_send_message (socket,
                             NULL, /* address */
                             &vector,
                             1,
                             &scm,
                             1,
                             G_SOCKET_MSG_NONE,
                             cancellable,
                             error) != 1)
    {
      g_prefix_error (error, _("Error sending credentials: "));
      goto out;
    }

  ret = TRUE;

 out:
  g_object_unref (socket);
  g_object_unref (scm);
  g_object_unref (credentials);
  return ret;
}

/**
 * g_unix_connection_receive_credentials:
 * @connection: A #GUnixConnection.
 * @cancellable: A #GCancellable or %NULL.
 * @error: Return location for error or %NULL.
 *
 * Receives credentials from the sending end of the connection.  The
 * sending end has to call g_unix_connection_send_credentials() (or
 * similar) for this to work.
 *
 * As well as reading the credentials this also reads (and discards) a
 * single byte from the stream, as this is required for credentials
 * passing to work on some implementations.
 *
 * Other ways to exchange credentials with a foreign peer includes the
 * #GUnixCredentialsMessage type and g_socket_get_credentials() function.
 *
 * Returns: (transfer full): Received credentials on success (free with
 * g_object_unref()), %NULL if @error is set.
 *
 * Since: 2.26
 */
GCredentials *
g_unix_connection_receive_credentials (GUnixConnection      *connection,
                                       GCancellable         *cancellable,
                                       GError              **error)
{
  GCredentials *ret;
  GSocketControlMessage **scms;
  gint nscm;
  GSocket *socket;
  gint n;
  volatile GType credentials_message_gtype;
  gssize num_bytes_read;
#ifdef __linux__
  gboolean turn_off_so_passcreds;
#endif

  g_return_val_if_fail (G_IS_UNIX_CONNECTION (connection), NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  ret = NULL;
  scms = NULL;

  g_object_get (connection, "socket", &socket, NULL);

  /* On Linux, we need to turn on SO_PASSCRED if it isn't enabled
   * already. We also need to turn it off when we're done.  See
   * #617483 for more discussion.
   */
#ifdef __linux__
  {
    gint opt_val;
    socklen_t opt_len;

    turn_off_so_passcreds = FALSE;
    opt_val = 0;
    opt_len = sizeof (gint);
    if (getsockopt (g_socket_get_fd (socket),
                    SOL_SOCKET,
                    SO_PASSCRED,
                    &opt_val,
                    &opt_len) != 0)
      {
        g_set_error (error,
                     G_IO_ERROR,
                     g_io_error_from_errno (errno),
                     _("Error checking if SO_PASSCRED is enabled for socket: %s"),
                     strerror (errno));
        goto out;
      }
    if (opt_len != sizeof (gint))
      {
        g_set_error (error,
                     G_IO_ERROR,
                     G_IO_ERROR_FAILED,
                     _("Unexpected option length while checking if SO_PASSCRED is enabled for socket. "
                       "Expected %d bytes, got %d"),
                     (gint) sizeof (gint), (gint) opt_len);
        goto out;
      }
    if (opt_val == 0)
      {
        opt_val = 1;
        if (setsockopt (g_socket_get_fd (socket),
                        SOL_SOCKET,
                        SO_PASSCRED,
                        &opt_val,
                        sizeof opt_val) != 0)
          {
            g_set_error (error,
                         G_IO_ERROR,
                         g_io_error_from_errno (errno),
                         _("Error enabling SO_PASSCRED: %s"),
                         strerror (errno));
            goto out;
          }
        turn_off_so_passcreds = TRUE;
      }
  }
#endif

  /* ensure the type of GUnixCredentialsMessage has been registered with the type system */
  credentials_message_gtype = G_TYPE_UNIX_CREDENTIALS_MESSAGE;
  num_bytes_read = g_socket_receive_message (socket,
                                             NULL, /* GSocketAddress **address */
                                             NULL,
                                             0,
                                             &scms,
                                             &nscm,
                                             NULL,
                                             cancellable,
                                             error);
  if (num_bytes_read != 1)
    {
      /* Handle situation where g_socket_receive_message() returns
       * 0 bytes and not setting @error
       */
      if (num_bytes_read == 0 && error != NULL && *error == NULL)
        {
          g_set_error_literal (error,
                               G_IO_ERROR,
                               G_IO_ERROR_FAILED,
                               _("Expecting to read a single byte for receiving credentials but read zero bytes"));
        }
      goto out;
    }

  if (nscm != 1)
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_FAILED,
		   _("Expecting 1 control message, got %d"),
                   nscm);
      goto out;
    }

  if (!G_IS_UNIX_CREDENTIALS_MESSAGE (scms[0]))
    {
      g_set_error_literal (error,
                           G_IO_ERROR,
                           G_IO_ERROR_FAILED,
			   _("Unexpected type of ancillary data"));
      goto out;
    }

  ret = g_unix_credentials_message_get_credentials (G_UNIX_CREDENTIALS_MESSAGE (scms[0]));
  g_object_ref (ret);

 out:

#ifdef __linux__
  if (turn_off_so_passcreds)
    {
      gint opt_val;
      opt_val = 0;
      if (setsockopt (g_socket_get_fd (socket),
                      SOL_SOCKET,
                      SO_PASSCRED,
                      &opt_val,
                      sizeof opt_val) != 0)
        {
          g_set_error (error,
                       G_IO_ERROR,
                       g_io_error_from_errno (errno),
                       _("Error while disabling SO_PASSCRED: %s"),
                       strerror (errno));
          goto out;
        }
    }
#endif

  if (scms != NULL)
    {
      for (n = 0; n < nscm; n++)
        g_object_unref (scms[n]);
      g_free (scms);
    }
  g_object_unref (socket);
  return ret;
}
