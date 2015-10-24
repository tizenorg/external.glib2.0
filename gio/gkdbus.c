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

#include "config.h"

#include <stdio.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#ifdef HAVE_SYS_FILIO_H
# include <sys/filio.h>
#endif

#ifdef HAVE_SYS_UIO_H
#include <sys/uio.h>
#endif

#include <linux/kdbus.h>
#include "gkdbus.h"
#include "glib-unix.h"
#include "glibintl.h"

#include "gcancellable.h"
#include "ginitable.h"
#include "gioenums.h"
#include "gioerror.h"
#include "gdbusmessage.h"
#include "gdbusconnection.h"
#include "gdbusutils.h"
#include "gunixfdlist.h"
#include "glib/gstdio.h"

#define RECEIVE_POOL_SIZE_DEFAULT_SIZE    (2 * 1024LU * 1024LU)
#define RECEIVE_POOL_SIZE_ENV_VAR_NAME    "KDBUS_MEMORY_POOL_SIZE"
#define RECEIVE_POOL_SIZE_MAX_MBYTES      64
#define RECEIVE_POOL_SIZE_MIN_KBYTES      16

#define MEMFD_SIZE_THRESHOLD              (512 * 1024LU)
#define KDBUS_REPLY_TIMEOUT               50000000000ULL
#define KDBUS_MSG_MAX_PAYLOAD_VEC_SIZE    0x00200000

#define KDBUS_ALIGN8(l)                   (((l) + 7) & ~7)
#define KDBUS_ALIGN8_PTR(p)               ((void*) (uintptr_t)(p))

#define KDBUS_ITEM_HEADER_SIZE            G_STRUCT_OFFSET(struct kdbus_item, data)
#define KDBUS_ITEM_SIZE(s)                KDBUS_ALIGN8(KDBUS_ITEM_HEADER_SIZE + (s))

#define KDBUS_ITEM_NEXT(item)     \
        (typeof(item))(((guint8 *)item) + KDBUS_ALIGN8((item)->size))
#define KDBUS_ITEM_FOREACH(item, head, first)                  \
        for (item = (head)->first;                             \
          (guint8 *)(item) < (guint8 *)(head) + (head)->size;  \
          item = KDBUS_ITEM_NEXT(item))

#define DBUS_ERROR_ACCESS_DENIED                "org.freedesktop.DBus.Error.AccessDenied"
#define DBUS_SERVICE_DBUS                       "org.freedesktop.DBus"
#define DBUS_PATH_DBUS                          "/org/freedesktop/DBus"

/* Owner flags */
#define DBUS_NAME_FLAG_ALLOW_REPLACEMENT        0x1 /* allow another service to become the primary owner if requested */
#define DBUS_NAME_FLAG_REPLACE_EXISTING         0x2 /* request to replace the current primary owner */
#define DBUS_NAME_FLAG_DO_NOT_QUEUE             0x4 /* if we can not become the primary owner do not place us in the queue */

/* Replies to request for a name */
#define DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER   1   /* service has become the primary owner of the requested name */
#define DBUS_REQUEST_NAME_REPLY_IN_QUEUE        2   /* service could not become the primary owner and has been placed in the queue */
#define DBUS_REQUEST_NAME_REPLY_EXISTS          3   /* service is already in the queue */
#define DBUS_REQUEST_NAME_REPLY_ALREADY_OWNER   4   /* service is already the primary owner */

/* Replies to releasing a name */
#define DBUS_RELEASE_NAME_REPLY_RELEASED        1   /* service was released from the given name */
#define DBUS_RELEASE_NAME_REPLY_NON_EXISTENT    2   /* the given name does not exist on the bus */
#define DBUS_RELEASE_NAME_REPLY_NOT_OWNER       3   /* service is not an owner of the given name */


static void     g_kdbus_initable_iface_init (GInitableIface  *iface);
static gboolean g_kdbus_initable_init       (GInitable       *initable,
                                             GCancellable    *cancellable,
                                             GError         **error);

#define g_kdbus_get_type _g_kdbus_get_type
G_DEFINE_TYPE_WITH_CODE (GKdbus, g_kdbus, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE,
                                               g_kdbus_initable_iface_init));

/* GKdbusPrivate struct */
struct _GKdbusPrivate
{
  gint            fd;
  gchar          *buffer_ptr;
  guint64         receive_pool_size;
  gchar          *my_DBus_uniqe_name;
  gint            unique_id;
  gint            memfd;
  guint64         __attribute__ ((__aligned__(8))) offset;
  gchar          *msg_buffer_ptr;
  GList          *matches;
  guint32         local_msg_serial;


  gint                *fds;
  gint                 num_fds;

  gsize                bloom_size;
  guint                bloom_n_hash;

  guint                registered : 1;
  guint                closed     : 1;
  guint                inited     : 1;
  guint                timed_out  : 1;
  guint                timeout;

#ifdef POLICY_IN_LIB
  PBusClientPolicy    *policy;
#endif
};

#ifdef POLICY_IN_LIB
static gboolean policy_check_can_send      (void          *policy,
                                            GDBusMessage  *message);
static gboolean send_local_cant_send_error (GDBusWorker   *worker,
                                            GKdbus        *kdbus,
                                            GDBusMessage  *dbus_msg);
#endif

/* GKdbusSource struct */
typedef struct {
  GSource       source;
  GPollFD       pollfd;
  GKdbus       *kdbus;
  GIOCondition  condition;
  GCancellable *cancellable;
  GPollFD       cancel_pollfd;
  gint64        timeout_time;
} GKdbusSource;


/*
 * Hash keys for bloom filters
 */
const guint8 hash_keys[8][16] =
{
  {0xb9,0x66,0x0b,0xf0,0x46,0x70,0x47,0xc1,0x88,0x75,0xc4,0x9c,0x54,0xb9,0xbd,0x15},
  {0xaa,0xa1,0x54,0xa2,0xe0,0x71,0x4b,0x39,0xbf,0xe1,0xdd,0x2e,0x9f,0xc5,0x4a,0x3b},
  {0x63,0xfd,0xae,0xbe,0xcd,0x82,0x48,0x12,0xa1,0x6e,0x41,0x26,0xcb,0xfa,0xa0,0xc8},
  {0x23,0xbe,0x45,0x29,0x32,0xd2,0x46,0x2d,0x82,0x03,0x52,0x28,0xfe,0x37,0x17,0xf5},
  {0x56,0x3b,0xbf,0xee,0x5a,0x4f,0x43,0x39,0xaf,0xaa,0x94,0x08,0xdf,0xf0,0xfc,0x10},
  {0x31,0x80,0xc8,0x73,0xc7,0xea,0x46,0xd3,0xaa,0x25,0x75,0x0f,0x9e,0x4c,0x09,0x29},
  {0x7d,0xf7,0x18,0x4b,0x7b,0xa4,0x44,0xd5,0x85,0x3c,0x06,0xe0,0x65,0x53,0x96,0x6d},
  {0xf2,0x77,0xe9,0x6f,0x93,0xb5,0x4e,0x71,0x9a,0x0c,0x34,0x88,0x39,0x25,0xbf,0x35}
};


/*
 * Enum for Match struct
 */
enum {
  MATCH_ELEMENT_TYPE,
  MATCH_ELEMENT_SENDER,
  MATCH_ELEMENT_INTERFACE,
  MATCH_ELEMENT_MEMBER,
  MATCH_ELEMENT_PATH,
  MATCH_ELEMENT_PATH_NAMESPACE,
  MATCH_ELEMENT_DESTINATION,
  MATCH_ELEMENT_ARG0NAMESPACE,
  MATCH_ELEMENT_EAVESDROP,
  MATCH_ELEMENT_ARGN,
  MATCH_ELEMENT_ARGNPATH,
};


/*
 * MatchElement struct
 */
typedef struct {
  guint16 type;
  guint16 arg;
  char *value;
} MatchElement;


/*
 * Match struct
 */
typedef struct {
  int n_elements;
  MatchElement *elements;
  gboolean eavesdrop;
  GDBusMessageType type;
  guint64 kdbus_cookie;
} Match;


/*
 * Macros for SipHash algorithm
 */
#define ROTL(x,b) (guint64)( ((x) << (b)) | ( (x) >> (64 - (b))) )

#define U32TO8_LE(p, v)         \
    (p)[0] = (guint8)((v)      ); (p)[1] = (guint8)((v) >>  8); \
    (p)[2] = (guint8)((v) >> 16); (p)[3] = (guint8)((v) >> 24);

#define U64TO8_LE(p, v)         \
  U32TO8_LE((p),     (guint32)((v)      ));   \
  U32TO8_LE((p) + 4, (guint32)((v) >> 32));

#define U8TO64_LE(p) \
  (((guint64)((p)[0])      ) | \
   ((guint64)((p)[1]) <<  8) | \
   ((guint64)((p)[2]) << 16) | \
   ((guint64)((p)[3]) << 24) | \
   ((guint64)((p)[4]) << 32) | \
   ((guint64)((p)[5]) << 40) | \
   ((guint64)((p)[6]) << 48) | \
   ((guint64)((p)[7]) << 56))

#define SIPROUND            \
  do {              \
    v0 += v1; v1=ROTL(v1,13); v1 ^= v0; v0=ROTL(v0,32); \
    v2 += v3; v3=ROTL(v3,16); v3 ^= v2;     \
    v0 += v3; v3=ROTL(v3,21); v3 ^= v0;     \
    v2 += v1; v1=ROTL(v1,17); v1 ^= v2; v2=ROTL(v2,32); \
  } while(0)


/*
 * SipHash algorithm
 */
static void
_g_siphash24 (guint8         out[8],
              const void    *_in,
              gsize          inlen,
              const guint8   k[16])
{
  guint64 v0 = 0x736f6d6570736575ULL;
  guint64 v1 = 0x646f72616e646f6dULL;
  guint64 v2 = 0x6c7967656e657261ULL;
  guint64 v3 = 0x7465646279746573ULL;
  guint64 b;
  guint64 k0 = U8TO64_LE (k);
  guint64 k1 = U8TO64_LE (k + 8);
  guint64 m;
  const guint8 *in = _in;
  const guint8 *end = in + inlen - (inlen % sizeof(guint64));
  const int left = inlen & 7;
  b = ((guint64) inlen) << 56;
  v3 ^= k1;
  v2 ^= k0;
  v1 ^= k1;
  v0 ^= k0;

  for (; in != end; in += 8)
    {
      m = U8TO64_LE (in);
      v3 ^= m;
      SIPROUND;
      SIPROUND;
      v0 ^= m;
    }

  switch (left)
    {
      case 7: b |= ((guint64) in[6]) << 48;
      case 6: b |= ((guint64) in[5]) << 40;
      case 5: b |= ((guint64) in[4]) << 32;
      case 4: b |= ((guint64) in[3]) << 24;
      case 3: b |= ((guint64) in[2]) << 16;
      case 2: b |= ((guint64) in[1]) <<  8;
      case 1: b |= ((guint64) in[0]); break;
      case 0: break;
    }

  v3 ^= b;
  SIPROUND;
  SIPROUND;
  v0 ^= b;

  v2 ^= 0xff;
  SIPROUND;
  SIPROUND;
  SIPROUND;
  SIPROUND;
  b = v0 ^ v1 ^ v2  ^ v3;
  U64TO8_LE (out, b);
}


/*
 * address_in_buffer()
 */
static int
address_in_buffer (void                *buffer_ptr,
                   unsigned long long   size,
                   void                *address)
{
  unsigned int ptr = (unsigned int) buffer_ptr;
  unsigned int addr = (unsigned int) address;

  return ptr <= addr && addr < ptr+size;
}


/*
 * is_key()
 */
static gboolean
is_key (const char *key_start, const char *key_end, char *value)
{
  gsize len = strlen (value);

  if (len != key_end - key_start)
    return FALSE;

  return strncmp (key_start, value, len) == 0;
}


/*
 * parse_key()
 */
static gboolean
parse_key (MatchElement *element, const char *key_start, const char *key_end)
{
  gboolean res = TRUE;

  if (is_key (key_start, key_end, "type"))
    {
      element->type = MATCH_ELEMENT_TYPE;
    }
  else if (is_key (key_start, key_end, "sender"))
    {
      element->type = MATCH_ELEMENT_SENDER;
    }
  else if (is_key (key_start, key_end, "interface"))
    {
      element->type = MATCH_ELEMENT_INTERFACE;
    }
  else if (is_key (key_start, key_end, "member"))
    {
      element->type = MATCH_ELEMENT_MEMBER;
    }
  else if (is_key (key_start, key_end, "path"))
    {
      element->type = MATCH_ELEMENT_PATH;
    }
  else if (is_key (key_start, key_end, "path_namespace"))
    {
      element->type = MATCH_ELEMENT_PATH_NAMESPACE;
    }
  else if (is_key (key_start, key_end, "destination"))
    {
      element->type = MATCH_ELEMENT_DESTINATION;
    }
  else if (is_key (key_start, key_end, "arg0namespace"))
    {
      element->type = MATCH_ELEMENT_ARG0NAMESPACE;
    }
  else if (is_key (key_start, key_end, "eavesdrop"))
    {
      element->type = MATCH_ELEMENT_EAVESDROP;
    }
  else if (key_end - key_start > 3 && is_key (key_start, key_start + 3, "arg"))
    {
      const char *digits = key_start + 3;
      const char *end_digits = digits;

      while (end_digits < key_end && g_ascii_isdigit (*end_digits))
        end_digits++;

      if (end_digits == key_end) /* argN */
        {
          element->type = MATCH_ELEMENT_ARGN;
          element->arg = atoi (digits);
        }
      else if (is_key (end_digits, key_end, "path")) /* argNpath */
        {
          element->type = MATCH_ELEMENT_ARGNPATH;
          element->arg = atoi (digits);
        }
      else
        res = FALSE;
    }
  else
    res = FALSE;

  return res;
}


/*
 * parse_value()
 */
static const char *
parse_value (MatchElement *element, const char *s)
{
  char quote_char;
  GString *value;

  value = g_string_new ("");

  quote_char = 0;

  for (;*s; s++)
    {
      if (quote_char == 0)
        {
          switch (*s)
            {
            case '\'':
              quote_char = '\'';
              break;

            case ',':
              s++;
              goto out;

            case '\\':
              quote_char = '\\';
              break;

            default:
              g_string_append_c (value, *s);
              break;
            }
        }
      else if (quote_char == '\\')
        {
          /* \ only counts as an escape if escaping a quote mark */
          if (*s != '\'')
            g_string_append_c (value, '\\');

          g_string_append_c (value, *s);
          quote_char = 0;
        }
      else /* quote_char == ' */
        {
          if (*s == '\'')
            quote_char = 0;
          else
            g_string_append_c (value, *s);
        }
    }

 out:
  if (quote_char == '\\')
    g_string_append_c (value, '\\');
  else if (quote_char == '\'')
    {
      g_string_free (value, TRUE);
      return NULL;
    }

  element->value = g_string_free (value, FALSE);
  return s;
}


/*
 * match_new()
 */
static Match *
match_new (const char *str)
{
  Match *match;
  GArray *elements;
  const char *p;
  const char *key_start;
  const char *key_end;
  MatchElement element;
  gboolean eavesdrop;
  GDBusMessageType type;
  int i;
  static guint64 new_cookie = 1;

  eavesdrop = FALSE;
  type = G_DBUS_MESSAGE_TYPE_INVALID;
  elements = g_array_new (TRUE, TRUE, sizeof (MatchElement));

  p = str;

  while (*p != 0)
    {
      memset (&element, 0, sizeof (element));

      /* Skip initial whitespace */
      while (*p && g_ascii_isspace (*p))
        p++;

      key_start = p;

      /* Read non-whitespace non-equals chars */
      while (*p && *p != '=' && !g_ascii_isspace (*p))
        p++;

      key_end = p;

      /* Skip any whitespace after key */
      while (*p && g_ascii_isspace (*p))
        p++;

      if (key_start == key_end)
        continue; /* Allow trailing whitespace */

      if (*p != '=')
        goto error;

      ++p;

      if (!parse_key (&element, key_start, key_end))
        goto error;

      p = parse_value (&element, p);
      if (p == NULL)
        goto error;

      if (element.type == MATCH_ELEMENT_EAVESDROP)
        {
          if (strcmp (element.value, "true") == 0)
            eavesdrop = TRUE;
          else if (strcmp (element.value, "false") == 0)
            eavesdrop = FALSE;
          else
            {
              g_free (element.value);
              goto error;
            }
          g_free (element.value);
        }
      else if (element.type == MATCH_ELEMENT_TYPE)
        {
          if (strcmp (element.value, "signal") == 0)
            type = G_DBUS_MESSAGE_TYPE_SIGNAL;
          else if (strcmp (element.value, "method_call") == 0)
            type = G_DBUS_MESSAGE_TYPE_METHOD_CALL;
          else if (strcmp (element.value, "method_return") == 0)
            type = G_DBUS_MESSAGE_TYPE_METHOD_RETURN;
          else if (strcmp (element.value, "error") == 0)
            type = G_DBUS_MESSAGE_TYPE_ERROR;
          else
            {
              g_free (element.value);
              goto error;
            }
          g_free (element.value);
        }
      else
        g_array_append_val (elements, element);
    }

  match = g_new0 (Match, 1);
  match->n_elements = elements->len;
  match->elements = (MatchElement *)g_array_free (elements, FALSE);
  match->eavesdrop = eavesdrop;
  match->type = type;
  match->kdbus_cookie = new_cookie++;

  return match;

 error:
  for (i = 0; i < elements->len; i++)
    g_free (g_array_index (elements, MatchElement, i).value);
  g_array_free (elements, TRUE);
  return NULL;
}


/*
 * match_free()
 */
static void
match_free (Match *match)
{
  int i;
  for (i = 0; i < match->n_elements; i++)
    g_free (match->elements[i].value);
  g_free (match->elements);
  g_free (match);
}


/*
 * match_equal()
 */
static gboolean
match_equal (Match *a,
             Match *b)
{
  int i;

  if (a->eavesdrop != b->eavesdrop)
    return FALSE;
  if (a->type != b->type)
    return FALSE;
  if (a->n_elements != b->n_elements)
    return FALSE;
  for (i = 0; i < a->n_elements; i++)
    {
      if (a->elements[i].type != b->elements[i].type ||
          a->elements[i].arg != b->elements[i].arg ||
          strcmp (a->elements[i].value, b->elements[i].value) != 0)
        return FALSE;
    }
  return TRUE;
}


/*
 * message_get_argN()
 */
static const gchar *
message_get_argN (GDBusMessage *message,
                  int           n,
                  gboolean      allow_path)
{
  const gchar *ret;
  GVariant *body;

  ret = NULL;

  body = g_dbus_message_get_body (message);

  if (body != NULL && g_variant_is_of_type (body, G_VARIANT_TYPE_TUPLE))
    {
      GVariant *item;

      item = g_variant_get_child_value (body, n);
      if (g_variant_is_of_type (item, G_VARIANT_TYPE_STRING) ||
          (allow_path && g_variant_is_of_type (item, G_VARIANT_TYPE_OBJECT_PATH)))
        ret = g_variant_get_string (item, NULL);
      g_variant_unref (item);
    }

  return ret;
}


/*
 * kdbus_source_prepare()
 */
static gboolean
kdbus_source_prepare (GSource  *source,
                      gint     *timeout)
{
  GKdbusSource *kdbus_source = (GKdbusSource *)source;

  if (g_cancellable_is_cancelled (kdbus_source->cancellable))
    return TRUE;

  if (kdbus_source->timeout_time)
    {
      gint64 now;

      now = g_source_get_time (source);

      *timeout = (kdbus_source->timeout_time - now + 999) / 1000;
      if (*timeout < 0)
        {
          kdbus_source->kdbus->priv->timed_out = TRUE;
          *timeout = 0;
          return TRUE;
        }
    }
  else
    *timeout = -1;

  if ((kdbus_source->condition & kdbus_source->pollfd.revents) != 0)
    return TRUE;

  return FALSE;
}


/*
 * kdbus_source_check()
 */
static gboolean
kdbus_source_check (GSource  *source)
{
  gint timeout;

  return kdbus_source_prepare (source, &timeout);
}


/*
 * kdbus_source_dispatch()
 */
static gboolean
kdbus_source_dispatch  (GSource      *source,
                        GSourceFunc   callback,
                        gpointer      user_data)
{
  GKdbusSourceFunc func = (GKdbusSourceFunc)callback;
  GKdbusSource *kdbus_source = (GKdbusSource *)source;
  GKdbus *kdbus = kdbus_source->kdbus;
  gboolean ret;

  if (kdbus_source->kdbus->priv->timed_out)
    kdbus_source->pollfd.revents |= kdbus_source->condition & (G_IO_IN | G_IO_OUT);

  ret = (*func) (kdbus,
                 kdbus_source->pollfd.revents & kdbus_source->condition,
                 user_data);

  if (kdbus->priv->timeout)
    kdbus_source->timeout_time = g_get_monotonic_time ()
                               + kdbus->priv->timeout * 1000000;
  else
    kdbus_source->timeout_time = 0;

  return ret;
}


/*
 * kdbus_source_finalize()
 */
static void
kdbus_source_finalize (GSource  *source)
{
  GKdbusSource *kdbus_source = (GKdbusSource *)source;
  GKdbus *kdbus;

  kdbus = kdbus_source->kdbus;

  g_object_unref (kdbus);

  if (kdbus_source->cancellable)
    {
      g_cancellable_release_fd (kdbus_source->cancellable);
      g_object_unref (kdbus_source->cancellable);
    }
}


/*
 * kdbus_source_closure_callback()
 */
static gboolean
kdbus_source_closure_callback (GKdbus        *kdbus,
                               GIOCondition   condition,
                               gpointer       data)
{
  GClosure *closure = data;
  GValue params[2] = { G_VALUE_INIT, G_VALUE_INIT };
  GValue result_value = G_VALUE_INIT;
  gboolean result;

  g_value_init (&result_value, G_TYPE_BOOLEAN);

  g_value_init (&params[0], G_TYPE_KDBUS);
  g_value_set_object (&params[0], kdbus);
  g_value_init (&params[1], G_TYPE_IO_CONDITION);
  g_value_set_flags (&params[1], condition);

  g_closure_invoke (closure, &result_value, 2, params, NULL);

  result = g_value_get_boolean (&result_value);
  g_value_unset (&result_value);
  g_value_unset (&params[0]);
  g_value_unset (&params[1]);

  return result;
}

static GSourceFuncs kdbus_source_funcs =
{
  kdbus_source_prepare,
  kdbus_source_check,
  kdbus_source_dispatch,
  kdbus_source_finalize,
  (GSourceFunc)kdbus_source_closure_callback,
};


/*
 * kdbus_source_new()
 */
static GSource *
kdbus_source_new (GKdbus        *kdbus,
                  GIOCondition   condition,
                  GCancellable  *cancellable)
{
  GSource *source;
  GKdbusSource *kdbus_source;

  source = g_source_new (&kdbus_source_funcs, sizeof (GKdbusSource));
  g_source_set_name (source, "GKdbus");
  kdbus_source = (GKdbusSource *)source;

  kdbus_source->kdbus = g_object_ref (kdbus);
  kdbus_source->condition = condition;

  if (g_cancellable_make_pollfd (cancellable,
                                 &kdbus_source->cancel_pollfd))
    {
      kdbus_source->cancellable = g_object_ref (cancellable);
      g_source_add_poll (source, &kdbus_source->cancel_pollfd);
    }

  kdbus_source->pollfd.fd = kdbus->priv->fd;
  kdbus_source->pollfd.events = condition;
  kdbus_source->pollfd.revents = 0;
  g_source_add_poll (source, &kdbus_source->pollfd);

  if (kdbus->priv->timeout)
    kdbus_source->timeout_time = g_get_monotonic_time ()
                               + kdbus->priv->timeout * 1000000;
  else
    kdbus_source->timeout_time = 0;

  return source;
}


/*
 * _g_kdbus_create_source()
 */
GSource *
_g_kdbus_create_source (GKdbus        *kdbus,
                        GIOCondition   condition,
                        GCancellable  *cancellable)
{
  g_return_val_if_fail (G_IS_KDBUS (kdbus) && (cancellable == NULL || G_IS_CANCELLABLE (cancellable)), NULL);

  return kdbus_source_new (kdbus, condition, cancellable);
}


/*
 * g_kdbus_finalize()
 */
static void
g_kdbus_finalize (GObject *object)
{
  GKdbus *kdbus = G_KDBUS (object);

  if (kdbus->priv->fd != -1 && !kdbus->priv->closed)
    g_kdbus_close (kdbus, NULL);

#ifdef POLICY_IN_LIB
  dbus_policy_free (kdbus->priv->policy);
#endif

  if (G_OBJECT_CLASS (g_kdbus_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_kdbus_parent_class)->finalize) (object);

}


/*
 * g_kdbus_class_init()
 */
static void
g_kdbus_class_init (GKdbusClass *klass)
{
  GObjectClass *gobject_class G_GNUC_UNUSED = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (GKdbusPrivate));
  gobject_class->finalize = g_kdbus_finalize;
}


/*
 * g_kdbus_initable_iface_init()
 */
static void
g_kdbus_initable_iface_init (GInitableIface *iface)
{
  iface->init = g_kdbus_initable_init;
}


/*
 * g_kdbus_init()
 */
static void
g_kdbus_init (GKdbus *kdbus)
{
  kdbus->priv = G_TYPE_INSTANCE_GET_PRIVATE (kdbus, G_TYPE_KDBUS, GKdbusPrivate);

  kdbus->priv->fd = -1;

  kdbus->priv->unique_id = -1;
  kdbus->priv->buffer_ptr = NULL;
  kdbus->priv->receive_pool_size = 0;
  kdbus->priv->my_DBus_uniqe_name = NULL;
  kdbus->priv->memfd = -1;
  kdbus->priv->matches = NULL;
  kdbus->priv->local_msg_serial = 1;

  kdbus->priv->fds = NULL;
  kdbus->priv->num_fds = 0;

#ifdef POLICY_IN_LIB
  kdbus->priv->policy = NULL;
#endif
}


/*
 * g_kdbus_initable_init()
 */
static gboolean
g_kdbus_initable_init (GInitable *initable,
                       GCancellable *cancellable,
                       GError  **error)
{
  GKdbus  *kdbus;

  g_return_val_if_fail (G_IS_KDBUS (initable), FALSE);

  kdbus = G_KDBUS (initable);

  if (cancellable != NULL)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           _("Cancellable initialization not supported"));
      return FALSE;
    }

  kdbus->priv->inited = TRUE;

  return TRUE;
}


/*
 * g_kdbus_open()
 */
gboolean
g_kdbus_open (GKdbus         *kdbus,
              const gchar    *address,
              GCancellable   *cancellable,
              GError        **error)
{
#ifdef POLICY_IN_LIB
  PBusConfigParser *config = NULL;
#endif

  g_return_val_if_fail (G_IS_KDBUS (kdbus), FALSE);

  g_debug ("[KDBUS] opening %s endpoint file", address);
  kdbus->priv->fd = open (address, O_RDWR|O_CLOEXEC|O_NONBLOCK|O_NOCTTY);
  if (kdbus->priv->fd<0)
  {
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, _("Can't open endpoint file"));
    g_debug ("[KDBUS] error %m, %d when opening address %s", errno, address);
    return FALSE;
  }

  g_debug ("[KDBUS] kdbus endpoint opened");
  kdbus->priv->closed = FALSE;

#ifdef POLICY_IN_LIB
  if (g_strstr_len(address, -1, "system"))
    {
      config = dbus_config_init (SYSTEM_BUS);
      kdbus->priv->policy = dbus_policy_init (config);
      g_debug ("[KDBUS] policy init for 'system' bus");
    }
  else if (g_strstr_len(address, -1, "kdbus-"))
    {
      g_debug ("[KDBUS] no policy used, bus name was uid-kdbus-pid");
    }
  else
    {
      config = dbus_config_init (SESSION_BUS);
      kdbus->priv->policy = dbus_policy_init (config);
      g_debug ("[KDBUS] policy init for 'session' bus");
    }

  dbus_config_free (config);
  if(kdbus->priv->policy == NULL)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, _("Initializing policies error"));
      g_debug ("[KDBUS] initializing policies error");
      return FALSE;
    }
#endif

  return TRUE;
}


/*
 * g_kdbus_close()
 */
gboolean
g_kdbus_close (GKdbus   *kdbus,
               GError  **error)
{
  GList *l;
  gboolean retvalue = TRUE;

  g_return_val_if_fail (G_IS_KDBUS (kdbus), FALSE);

  if (ioctl(kdbus->priv->fd, KDBUS_CMD_BYEBYE) < 0)
  {
      retvalue = FALSE;
  }

  close(kdbus->priv->fd);

  if (kdbus->priv->buffer_ptr != NULL && kdbus->priv->buffer_ptr != MAP_FAILED)
    {
      if (munmap (kdbus->priv->buffer_ptr, kdbus->priv->receive_pool_size) == -1)
        {
          g_warning ("%s(): error when munmap: %m, %d",__FUNCTION__,errno);
          g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
             _("Error when munmap: %s"), g_strerror (errno));
          /* we cannot return FALSE here because fd was already closed few lines before */
        }
    }

  kdbus->priv->buffer_ptr = NULL;

  kdbus->priv->closed = TRUE;
  kdbus->priv->fd = -1;
  kdbus->priv->registered = FALSE;

  for (l = kdbus->priv->matches; l != NULL; l = l->next)
    {
      if(l->data != NULL)
        match_free (l->data);
    }
  g_list_free (kdbus->priv->matches);

  free(kdbus->priv->my_DBus_uniqe_name);
  g_debug ("[KDBUS] kdbus endpoint closed");

  return retvalue;
}

/*
 * g_kdbus_is_closed()
 */
gboolean
g_kdbus_is_closed (GKdbus *kdbus)
{
  g_return_val_if_fail (G_IS_KDBUS (kdbus), FALSE);

  return kdbus->priv->closed;
}


/*
 * g_kdbus_bloom_add_data()
 * Based on bus-bloom.c from systemd
 * http://cgit.freedesktop.org/systemd/systemd/tree/src/libsystemd/sd-bus/bus-bloom.c
 */
static void
g_kdbus_bloom_add_data (GKdbus        *kdbus,
                        guint64        bloom_data [],
                        const void    *data,
                        gsize          n)
{
  guint8 hash[8];
  guint64 bit_num;
  guint bytes_num = 0;
  guint cnt_1, cnt_2;
  guint hash_index = 0;

  guint c = 0;
  guint64 p = 0;

  bit_num = kdbus->priv->bloom_size * 8;

  if (bit_num > 1)
    bytes_num = ((__builtin_clzll(bit_num) ^ 63U) + 7) / 8;

  for (cnt_1 = 0; cnt_1 < (kdbus->priv->bloom_n_hash); cnt_1++)
    {
      for (cnt_2 = 0, hash_index = 0; cnt_2 < bytes_num; cnt_2++)
        {
          if (c <= 0)
            {
              _g_siphash24(hash, data, n, hash_keys[hash_index++]);
              c += 8;
            }

          p = (p << 8ULL) | (guint64) hash[8 - c];
          c--;
        }

      p &= bit_num - 1;
      bloom_data[p >> 6] |= 1ULL << (p & 63);
    }
}


/*
 * g_kdbus_bloom_add_pair()
 */
static void
g_kdbus_bloom_add_pair (GKdbus        *kdbus,
                        guint64        bloom_data [],
                        const gchar   *parameter,
                        const gchar   *value)
{
  gchar buf[1024];
  gsize size;

  size = strlen(parameter) + strlen(value) + 1;
  if (size > 1024)
    return;

  strcpy(stpcpy(stpcpy(buf, parameter), ":"), value);
  g_kdbus_bloom_add_data (kdbus, bloom_data, buf, size);
}


/*
 * g_kdbus_bloom_add_prefixes()
 */
static void
g_kdbus_bloom_add_prefixes (GKdbus        *kdbus,
                            guint64        bloom_data [],
                            const gchar   *parameter,
                            const gchar   *value,
                            gchar          separator)
{
  gchar buf[1024];
  gsize size;

  size = strlen(parameter) + strlen(value) + 1;
  if (size > 1024)
    return;

  strcpy(stpcpy(stpcpy(buf, parameter), ":"), value);

  for (;;)
    {
      gchar *last_sep;
      last_sep = strrchr(buf, separator);
      if (!last_sep || last_sep == buf)
        break;

      *last_sep = 0;
      g_kdbus_bloom_add_data (kdbus, bloom_data, buf, last_sep-buf);
    }
}


/*
 * g_kdbus_register()
 */
static gboolean
g_kdbus_register (GKdbus *kdbus)
{
  struct kdbus_cmd_hello hello;
  __u64 receive_pool_size = RECEIVE_POOL_SIZE_DEFAULT_SIZE;
  const gchar *env_pool;

  memset(&hello, 0, sizeof(struct kdbus_cmd_hello));
  hello.conn_flags = KDBUS_HELLO_ACCEPT_FD;
  hello.attach_flags = 0;
  hello.size = sizeof(struct kdbus_cmd_hello);

  env_pool = getenv (RECEIVE_POOL_SIZE_ENV_VAR_NAME);
  if(env_pool)
    {
      guint64 size;
      guint32 multiply = 1;
      gint64 page_size;

      page_size = sysconf(_SC_PAGESIZE);
      if(page_size == -1)
        {
          size = 0;
          goto finish;
        }

      errno = 0;
      size = strtoul(env_pool, (char**)&env_pool, 10);
      if(errno == EINVAL || size == 0)
        {
          size = 0;
          goto finish;
        }

      if(*env_pool == 'k')
        {
          multiply = 1024;
          env_pool++;
        }
      else if (*env_pool == 'M')
        {
          multiply = 1024 * 1024;
          env_pool++;
        }

      if(*env_pool != '\0')
        {
          size = 0;
          goto finish;
        }

      receive_pool_size = size * multiply;

      if((receive_pool_size > RECEIVE_POOL_SIZE_MAX_MBYTES * 1024 * 1024) ||
         (receive_pool_size < RECEIVE_POOL_SIZE_MIN_KBYTES * 1024) ||
         ((receive_pool_size & (page_size - 1)) != 0))
        size = 0;

    finish:
      if(size == 0)
        {
          g_warning ("%s value is invalid, default value %luB will be used.", RECEIVE_POOL_SIZE_ENV_VAR_NAME,
                      RECEIVE_POOL_SIZE_DEFAULT_SIZE);
          g_warning ("Correct value must be between %ukB and %uMB and must be aligned to page size: %" G_GINT64_FORMAT "B.",
                      RECEIVE_POOL_SIZE_MIN_KBYTES, RECEIVE_POOL_SIZE_MAX_MBYTES, page_size);

          receive_pool_size = RECEIVE_POOL_SIZE_DEFAULT_SIZE;
        }
    }

  g_debug ("[KDBUS] receive pool size set to %llu", receive_pool_size);
  kdbus->priv->receive_pool_size = receive_pool_size;
  hello.pool_size = receive_pool_size;

  if (ioctl(kdbus->priv->fd, KDBUS_CMD_HELLO, &hello) < 0)
    {
      g_warning ("Registration (Hello) on the bus failed: %m, %d", errno);
      return FALSE;
    }

  kdbus->priv->registered = TRUE;

  kdbus->priv->unique_id = hello.id;
  if (asprintf(&kdbus->priv->my_DBus_uniqe_name, ":1.%020llu", (unsigned long long)hello.id) == -1)
    return FALSE;

  kdbus->priv->buffer_ptr = mmap (NULL, receive_pool_size, PROT_READ, MAP_SHARED, kdbus->priv->fd, 0);
  if (kdbus->priv->buffer_ptr == MAP_FAILED)
    {
      g_warning ("Error when mmap after Hello: %m, %d", errno);
      free (kdbus->priv->my_DBus_uniqe_name);
      return FALSE;
    }

  kdbus->priv->bloom_size = hello.bloom.size;
  kdbus->priv->bloom_n_hash = hello.bloom.n_hash;

  g_debug ("[KDBUS] our unique ID: %llu", hello.id);
  return TRUE;
}


/*
 * g_kdbus_request_name()
 */
static gboolean
g_kdbus_request_name (GKdbus        *kdbus,
                      const gchar   *name,
                      guint32       *flags,
                      GError       **error)
{
  struct kdbus_cmd_name *cmd_name;
  __u64 size;
  __u64 flags_kdbus = 0;

  if (!g_dbus_is_name (name))
    {
      g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                   "Requested bus name \"%s\" is not valid", name);
      g_debug ("[KDBUS] requested bus name \"%s\" is not valid", name);
      return FALSE;
    }

  if (*name == ':')
    {
      g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                   "Cannot acquire a service starting with ':' such as \"%s\"", name);
      g_debug ("[KDBUS] cannot acquire a service starting with ':'");
      return FALSE;
    }

  if (strcmp (name, "org.freedesktop.DBus") == 0)
    {
      g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                   "Cannot acquire a service named org.freedesktop.DBus, because that is reserved");
      g_debug ("[KDBUS] cannot acquire a service named org.freedesktop.DBus");
      return FALSE;
    }

#ifdef POLICY_IN_LIB
  if (!dbus_policy_check_can_own (kdbus->priv->policy, name))
    {
      g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED,
                   "Connection \"%s\" is not allowed to own the service \"%s\" due "
                   "to security policies in the configuration file",
                   kdbus->priv->my_DBus_uniqe_name, name);
      g_debug ("[KDBUS] policy 'own' checked - I can't own name %s!", name);
      return FALSE;
    }
#endif

  size = sizeof(*cmd_name) + strlen(name) + 1;
  cmd_name = alloca(size);

  strcpy(cmd_name->name, name);
  cmd_name->size = size;

  if(*flags & DBUS_NAME_FLAG_ALLOW_REPLACEMENT)
   flags_kdbus |= KDBUS_NAME_ALLOW_REPLACEMENT;
  if(!(*flags & DBUS_NAME_FLAG_DO_NOT_QUEUE))
   flags_kdbus |= KDBUS_NAME_QUEUE;
  if(*flags & DBUS_NAME_FLAG_REPLACE_EXISTING)
   flags_kdbus |= KDBUS_NAME_REPLACE_EXISTING;

  cmd_name->flags = flags_kdbus;
  *flags = 0;

  if (ioctl(kdbus->priv->fd, KDBUS_CMD_NAME_ACQUIRE, cmd_name) < 0)
   {
     if(errno == EEXIST)
       {
         *flags = DBUS_REQUEST_NAME_REPLY_EXISTS;
         return TRUE;
       }
     else if(errno == EALREADY)
       {
         *flags = DBUS_REQUEST_NAME_REPLY_ALREADY_OWNER;
         return TRUE;
       }
     else
       {
         g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno), _("Error when requesting for name, %s"), g_strerror (errno));
         g_debug ("[KDBUS] error requesting name: '%s'", name);
         return FALSE;
       }
   }

  if(cmd_name->flags & KDBUS_NAME_IN_QUEUE)
   *flags = DBUS_REQUEST_NAME_REPLY_IN_QUEUE;
  else
   *flags = DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER;

  return TRUE;
}


/*
 * g_kdbus_release_name()
 */
static guint32
g_kdbus_release_name (GKdbus       *kdbus,
                      const gchar  *name,
                      GError      **error)
{
  struct kdbus_cmd_name *cmd_name;
  __u64 size;

  if (!g_dbus_is_name (name))
    {
      g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                   "Given bus name \"%s\" is not valid", name);
      g_debug ("[KDBUS] given bus name \"%s\" is not valid", name);
      return 0;
    }

  if (*name == ':')
    {
      g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                   "Attempt to release invalid base service name \"%s\"", name);
      g_debug ("[KDBUS] attempt to release invalid base service name \"%s\"", name);
      return 0;
    }

  if (strcmp (name, "org.freedesktop.DBus") == 0)
    {
      g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                   "Cannot release the %s service because it is owned by the bus", name);
      g_debug ("[KDBUS] cannot release the %s service because it is owned by the bus", name);
      return 0;
    }

  size = sizeof(*cmd_name) + strlen(name) + 1;
  cmd_name = alloca(size);
  strcpy(cmd_name->name, name);
  cmd_name->size = size;

  if (ioctl(kdbus->priv->fd, KDBUS_CMD_NAME_RELEASE, cmd_name))
    {
      if((errno == ESRCH) || (errno == ENXIO))
        return DBUS_RELEASE_NAME_REPLY_NON_EXISTENT;
      else if (errno == EADDRINUSE)
        return DBUS_RELEASE_NAME_REPLY_NOT_OWNER;

      g_set_error (error, G_DBUS_ERROR, g_io_error_from_errno (errno), _("Error when releasing name, %s"), g_strerror (errno));
      g_debug ("[KDBUS] error releasing name '%s'", name);
      return 0;
    }

  g_debug ("[KDBUS] name '%s' released", name);
  return DBUS_RELEASE_NAME_REPLY_RELEASED;
}


/*
 * g_kdbus_get_buffer_ptr()
 */
gchar *
g_kdbus_get_msg_buffer_ptr (GKdbus  *kdbus)
{
  g_return_val_if_fail (G_IS_KDBUS (kdbus), NULL);

  return kdbus->priv->msg_buffer_ptr;
}


/*
 * g_kdbus_append_payload_vec()
 */
static void
g_kdbus_append_payload_vec (struct kdbus_item **item,
                            const void         *data_ptr,
                            gssize              size)
{
  *item = KDBUS_ALIGN8_PTR(*item);
  (*item)->size = G_STRUCT_OFFSET (struct kdbus_item, vec) + sizeof(struct kdbus_vec);
  (*item)->type = KDBUS_ITEM_PAYLOAD_VEC;
  (*item)->vec.address = (guint64)((guintptr)data_ptr);
  (*item)->vec.size = size;
  *item = KDBUS_ITEM_NEXT(*item);
}


/*
 * g_kdbus_append_payload_memfd()
 */
static void
g_kdbus_append_payload_memfd (struct kdbus_item **item,
                              gint                memfd,
                              gsize               size)
{
  *item = KDBUS_ALIGN8_PTR(*item);
  (*item)->size = KDBUS_ITEM_HEADER_SIZE + sizeof(struct kdbus_memfd);
  (*item)->type = KDBUS_ITEM_PAYLOAD_MEMFD;

  (*item)->memfd.size = size;
  (*item)->memfd.fd = memfd;
  *item = KDBUS_ITEM_NEXT(*item);
}


/*
 * g_kdbus_append_destiantion()
 */
static void
g_kdbus_append_destination (struct kdbus_item **item,
                            const gchar        *destination,
                            gsize               size)
{
  *item = KDBUS_ALIGN8_PTR(*item);
  (*item)->size = G_STRUCT_OFFSET (struct kdbus_item, str) + size + 1;
  (*item)->type = KDBUS_ITEM_DST_NAME;
  memcpy ((*item)->str, destination, size+1);
  *item = KDBUS_ITEM_NEXT(*item);
}


/*
 * g_kdbus_append_bloom()
 */
static struct kdbus_bloom_filter *
g_kdbus_append_bloom (struct kdbus_item **item,
                      gsize               size)
{
  struct kdbus_item *bloom_item;

  bloom_item = KDBUS_ALIGN8_PTR(*item);
  bloom_item->size = G_STRUCT_OFFSET (struct kdbus_item, bloom_filter) +
                     G_STRUCT_OFFSET (struct kdbus_bloom_filter, data) +
                     size;

  bloom_item->type = KDBUS_ITEM_BLOOM_FILTER;

  *item = KDBUS_ITEM_NEXT(bloom_item);
  return &bloom_item->bloom_filter;
}


/*
 * g_kdbus_append_fds()
 */
static void
g_kdbus_append_fds (struct kdbus_item **item,
                    GUnixFDList        *fd_list)
{
  *item = KDBUS_ALIGN8_PTR(*item);
  (*item)->size = G_STRUCT_OFFSET (struct kdbus_item, fds) + sizeof(int) * g_unix_fd_list_get_length(fd_list);
  (*item)->type = KDBUS_ITEM_FDS;
  memcpy ((*item)->fds, g_unix_fd_list_peek_fds(fd_list, NULL), sizeof(int) * g_unix_fd_list_get_length(fd_list));

  *item = KDBUS_ITEM_NEXT(*item);
}


/*
 * _g_kdbus_attach_fds_to_msg()
 */
void
g_kdbus_attach_fds_to_msg (GKdbus       *kdbus,
                           GUnixFDList **fd_list)
{
  if ((kdbus->priv->fds != NULL) && (kdbus->priv->num_fds > 0))
    {
      gint n;

      if (*fd_list == NULL)
        *fd_list = g_unix_fd_list_new();

      for (n = 0; n < kdbus->priv->num_fds; n++)
        {
          g_unix_fd_list_append (*fd_list, kdbus->priv->fds[n], NULL);
          (void) g_close (kdbus->priv->fds[n], NULL);
        }

      g_free (kdbus->priv->fds);
      kdbus->priv->fds = NULL;
      kdbus->priv->num_fds = 0;
    }
}


/*
 * g_kdbus_setup_bloom:
 * Based on bus-bloom.c from systemd
 * http://cgit.freedesktop.org/systemd/systemd/tree/src/libsystemd/sd-bus/bus-bloom.c
 */
static void
g_kdbus_setup_bloom (GKdbus                     *kdbus,
                     GDBusMessage               *dbus_msg,
                     struct kdbus_bloom_filter  *bloom_filter)
{
  GVariant *body;
  GVariantIter iter;
  GVariant *child;

  const gchar *message_type;
  const gchar *interface;
  const gchar *member;
  const gchar *path;

  void *bloom_data;
  gint cnt = 0;

  body = g_dbus_message_get_body (dbus_msg);
  message_type = _g_dbus_enum_to_string (G_TYPE_DBUS_MESSAGE_TYPE, g_dbus_message_get_message_type (dbus_msg));
  interface = g_dbus_message_get_interface (dbus_msg);
  member = g_dbus_message_get_member (dbus_msg);
  path = g_dbus_message_get_path (dbus_msg);

  bloom_data = bloom_filter->data;
  memset (bloom_data, 0, kdbus->priv->bloom_size);
  bloom_filter->generation = 0;

  g_kdbus_bloom_add_pair(kdbus, bloom_data, "message-type", message_type);

  if (interface)
    g_kdbus_bloom_add_pair(kdbus, bloom_data, "interface", interface);

  if (member)
    g_kdbus_bloom_add_pair(kdbus, bloom_data, "member", member);

  if (path)
    {
      g_kdbus_bloom_add_pair(kdbus, bloom_data, "path", path);
      g_kdbus_bloom_add_pair(kdbus, bloom_data, "path-slash-prefix", path);
      g_kdbus_bloom_add_prefixes(kdbus, bloom_data, "path-slash-prefix", path, '/');
    }

  if (body != NULL)
    {
      g_variant_iter_init (&iter, body);
      while ((child = g_variant_iter_next_value (&iter)))
        {
          gchar buf[sizeof("arg")-1 + 2 + sizeof("-slash-prefix")];
          gchar *child_string;
          gchar *e;

          if (!(g_variant_is_of_type (child, G_VARIANT_TYPE_STRING)) &&
              !(g_variant_is_of_type (child, G_VARIANT_TYPE_OBJECT_PATH)) &&
              !(g_variant_is_of_type (child, G_VARIANT_TYPE_SIGNATURE)))
            break;

          child_string = g_variant_dup_string (child, NULL);

          e = stpcpy(buf, "arg");
          if (cnt < 10)
            *(e++) = '0' + (char) cnt;
          else
            {
              *(e++) = '0' + (char) (cnt / 10);
              *(e++) = '0' + (char) (cnt % 10);
            }

          *e = 0;
          g_kdbus_bloom_add_pair(kdbus, bloom_data, buf, child_string);

          strcpy(e, "-dot-prefix");
          g_kdbus_bloom_add_prefixes(kdbus, bloom_data, buf, child_string, '.');

          strcpy(e, "-slash-prefix");
          g_kdbus_bloom_add_prefixes(kdbus, bloom_data, buf, child_string, '/');

          g_free (child_string);
          g_variant_unref (child);
          cnt++;
        }
    }
}


/*
 * g_kdbus_acquire_memfd()
 */
static gint
g_kdbus_acquire_memfd (GKdbus   *kdbus,
                       guint64   size)
{
  gint ret;
  struct kdbus_cmd_memfd_make memfd;

  memfd.size = sizeof(struct kdbus_cmd_memfd_make);
  memfd.file_size = size;

  if ((ret = ioctl(kdbus->priv->fd, KDBUS_CMD_MEMFD_NEW, &memfd)) < 0)
    {
      g_warning ("KDBUS_CMD_MEMFD_NEW failed (%d): %m\n", ret);
      return -1;
    }

  g_debug ("[KDBUS] new memfd=%i", memfd.fd);
  return memfd.fd;
}


/*
 * get_next_local_msg_serial()
 */
static guint32
get_next_local_msg_serial (GKdbus *kdbus)
{
  guint32 serial;

  serial = kdbus->priv->local_msg_serial++;

  if (kdbus->priv->local_msg_serial == 0)
    kdbus->priv->local_msg_serial = 1;

  return serial;
}


/*
 * g_kdbus_generate_local_dbus_msg()
 */
static GDBusMessage *
g_kdbus_generate_local_dbus_msg (GDBusMessage      *message,
                                 GDBusMessageType   message_type,
                                 GDBusMessageFlags  message_flags,
                                 GVariant          *message_body,
                                 const gchar       *error_or_member,
                                 GKdbus            *kdbus)
{
  GDBusMessage *new_message;

  new_message = g_dbus_message_new ();

  g_dbus_message_set_sender (new_message, DBUS_SERVICE_DBUS);
  g_dbus_message_set_interface(new_message, DBUS_SERVICE_DBUS);
  g_dbus_message_set_path (new_message, DBUS_PATH_DBUS);
  g_dbus_message_set_message_type (new_message, message_type);
  g_dbus_message_set_flags (new_message, message_flags);
  g_dbus_message_set_body (new_message, message_body);
  g_dbus_message_set_serial(new_message, get_next_local_msg_serial(kdbus));
  g_dbus_message_set_destination(new_message, kdbus->priv->my_DBus_uniqe_name);

  if (message != NULL)
    g_dbus_message_set_reply_serial (new_message, g_dbus_message_get_serial (message));

  if (message_type == G_DBUS_MESSAGE_TYPE_ERROR)
    g_dbus_message_set_error_name (new_message, error_or_member);
  else if(error_or_member)
    g_dbus_message_set_member(new_message, error_or_member);

  if (G_UNLIKELY (_g_dbus_debug_message ()))
    {
      gchar *s;
      _g_dbus_debug_print_lock ();
      g_print ("========================================================================\n"
               "GDBus-debug:Message:\n"
               "  <<<< RECEIVED LOCAL D-Bus message (N/A bytes)\n");

      s = g_dbus_message_print (new_message, 2);
      g_print ("%s", s);
      g_free (s);
      _g_dbus_debug_print_unlock ();
    }

  return new_message;
}


/*
 * parse_sender()
 */
static gint
parse_sender(const gchar  *sender,
             __u64        *id)
{
  gchar *endptr;

  if(strncmp(sender, ":1.", 3) == 0)
    {
      *id = strtoull(&sender[3], &endptr, 10);
      if (*id == 0 || *endptr != '\0' || errno ==  ERANGE)
        return -1;
      else
        return 1;
    }
  else
    return 0;
}


/*
 * g_kdbus_addmatch()
 */
static gboolean
g_kdbus_addmatch(GKdbus  *kdbus,
                 Match   *match,
                 GError **error)
{
  struct kdbus_cmd_match    *msg;
  struct kdbus_item         *item;
  gint      i, sender = -1;
  gint      sender_size = 0;
  __u64     src_id = KDBUS_MATCH_ID_ANY;
  guint64   msg_size;
  guint64   *bloom;
  gboolean  need_bloom = FALSE;
  gchar     argument_buf[sizeof("arg")-1 + 2 + sizeof("-slash-prefix") +1];
  gchar     *string;
  gboolean  standard_rule_also = TRUE;

  /*
   * First check if it is org.freedesktop.DBus's NameOwnerChanged or any
   * org.freedesktop.DBus combination that includes this,
   * because it must be converted to special kdbus rule (kdbus has separate rules
   * for kdbus(kernel) generated broadcasts).
   */

  string = NULL;

  if (match->type != G_DBUS_MESSAGE_TYPE_SIGNAL)
    goto standard_rule;

  for (i=0; i<match->n_elements; i++)
     {
       switch(match->elements[i].type)
       {
         case MATCH_ELEMENT_SENDER:
           sender = parse_sender(match->elements[i].value, &src_id);
           if(sender < 0)
             return FALSE;
           if(sender == 0)
             {
               if (strcmp(match->elements[i].value, DBUS_SERVICE_DBUS))
                 goto standard_rule;
               else
                 standard_rule_also = FALSE;
             }
           if(sender > 0)
             {
               __u64 daemonsId;

               /* TODO: change to real daemon's id */
               daemonsId = 1;
               if (src_id != daemonsId)
                 goto standard_rule;
               else
                 standard_rule_also = FALSE;
             }
         break;

         case MATCH_ELEMENT_INTERFACE:
           if(strcmp(match->elements[i].value, DBUS_SERVICE_DBUS))
             goto standard_rule;
           else
             standard_rule_also = FALSE;
         break;

         case MATCH_ELEMENT_MEMBER:
           if(strcmp(match->elements[i].value, "NameOwnerChanged"))
             goto standard_rule;
         break;

         case MATCH_ELEMENT_PATH:
           if(strcmp(match->elements[i].value, DBUS_PATH_DBUS))
             goto standard_rule;
           else
             standard_rule_also = FALSE;
         break;

         /* TODO: could be implemented also, see use cases below 
         case MATCH_ELEMENT_ARGN:
           sprintf(argument_buf, "arg%d", match->elements->arg);
           bloom_add_pair(bloom, bloom_size, bloom_n_hash, argument_buf, match->elements[i].value);
           need_bloom = TRUE;
         break;
         */
       }
     }

  /* now we have to add kdbus rules related to well-known names */
  msg_size = KDBUS_ALIGN8(offsetof(struct kdbus_cmd_match, items) +
       offsetof(struct kdbus_item, name_change) +
       offsetof(struct kdbus_notify_name_change, name));

  msg = alloca(msg_size);
  if(msg == NULL)
    {
      errno = ENOMEM;
      return FALSE;
    }

  msg->cookie = match->kdbus_cookie;
  msg->size = msg_size;

  /* first match against any name change */
  item = msg->items;
  item->size =
       offsetof(struct kdbus_item, name_change) +
       offsetof(struct kdbus_notify_name_change, name);  /* TODO: name from arg0 can be added here (if present in the rule) */
  item->name_change.old.id = KDBUS_MATCH_ID_ANY;         /* TODO: can be replaced with arg0 or arg1 from rule (if present)  */
  item->name_change.new.id = KDBUS_MATCH_ID_ANY;         /* TODO: can be replaced with arg0 or arg2 from rule (if present)  */

  item->type = KDBUS_ITEM_NAME_CHANGE;
   if(ioctl(kdbus->priv->fd, KDBUS_CMD_MATCH_ADD, msg))
     {
       g_debug ("[KDBUS] failed adding match rule for name changes for daemon, error: %d, %m", errno);
       g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno), _("Error when adding match rule, %s"), g_strerror (errno));
       return FALSE;
     }

   /* then match against any name add */
   item->type = KDBUS_ITEM_NAME_ADD;
   if(ioctl(kdbus->priv->fd, KDBUS_CMD_MATCH_ADD, msg))
     {
       g_debug ("[KDBUS] failed adding match rule for name adding for daemon, error: %d, %m", errno);
       g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno), _("Error when adding match rule, %s"), g_strerror (errno));
       return FALSE;
     }

   /* then match against any name remove */
   item->type = KDBUS_ITEM_NAME_REMOVE;
   if(ioctl(kdbus->priv->fd, KDBUS_CMD_MATCH_ADD, msg))
     {
       g_debug ("[KDBUS] ailed adding match rule for name removal for daemon, error: %d, %m", errno);
       g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno), _("Error when adding match rule, %s"), g_strerror (errno));
       return FALSE;
     }

   /* now we add kdbus rules related to unique names */
   msg_size = KDBUS_ALIGN8(offsetof(struct kdbus_cmd_match, items) +
       offsetof(struct kdbus_item, id_change) +
       sizeof(struct kdbus_notify_id_change));

   msg = alloca(msg_size);
   if(msg == NULL)
     {
       errno = ENOMEM;
       return FALSE;
     }

   msg->cookie = match->kdbus_cookie;
   msg->size = msg_size;

   item = msg->items;
   item->size =
       offsetof(struct kdbus_item, id_change) +
       sizeof(struct kdbus_notify_id_change);
   item->id_change.id = KDBUS_MATCH_ID_ANY; /* TODO: can be replaced with arg0 or arg1 or arg2 from rule */

   item->type = KDBUS_ITEM_ID_ADD;
   if(ioctl(kdbus->priv->fd, KDBUS_CMD_MATCH_ADD, msg))
     {
       g_debug ("[KDBUS] failed adding match rule for adding id for daemon, error: %d, %m", errno);
       g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno), _("Error when adding match rule, %s"), g_strerror (errno));
       return FALSE;
     }

   item->type = KDBUS_ITEM_ID_REMOVE;
   if(ioctl(kdbus->priv->fd, KDBUS_CMD_MATCH_ADD, msg))
     {
       g_debug ("[KDBUS] failed adding match rule for id removal for daemon, error: %d, %m", errno);
       g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno), _("Error when adding match rule, %s"), g_strerror (errno));
       return FALSE;
     }

   g_debug ("[KDBUS] added match rule for kernel correctly");

   if(standard_rule_also == FALSE)
     return TRUE;

  /*
   * standard rule - registered in general way, for non-kernel broadcasts
   * kdbus don't use it to check kdbus(kernel) generated broadcasts
   */
standard_rule:

  bloom = alloca(kdbus->priv->bloom_size);
  if(bloom == NULL)
    return FALSE;

  memset(bloom, 0, kdbus->priv->bloom_size);
  msg_size = sizeof(struct kdbus_cmd_match);

  if (match->type != G_DBUS_MESSAGE_TYPE_INVALID)
    {
      string = _g_dbus_enum_to_string (G_TYPE_DBUS_MESSAGE_TYPE, match->type);
      g_kdbus_bloom_add_pair(kdbus, bloom, "message-type", string);
      g_free(string);
    }

  for (i=0; i<match->n_elements; i++)
    {
      switch(match->elements[i].type)
      {
        case MATCH_ELEMENT_SENDER:
          sender = parse_sender(match->elements[i].value, &src_id);
          if(sender < 0)
            return FALSE;

          if(sender > 0) /* unique_id */
            msg_size += KDBUS_ITEM_SIZE(sizeof(guint64));
          else           /* well-known name */
            {
              sender_size = strlen(match->elements[i].value) + 1;
              msg_size += KDBUS_ITEM_SIZE(sender_size);
              string = match->elements[i].value;
            }
        break;

        case MATCH_ELEMENT_INTERFACE:
          g_kdbus_bloom_add_pair(kdbus, bloom, "interface", match->elements[i].value);
          need_bloom = TRUE;
        break;

        case MATCH_ELEMENT_MEMBER:
          g_kdbus_bloom_add_pair(kdbus, bloom, "member", match->elements[i].value);
          need_bloom = TRUE;
        break;

        case MATCH_ELEMENT_PATH:
          g_kdbus_bloom_add_pair(kdbus, bloom, "path", match->elements[i].value);
          need_bloom = TRUE;
        break;

        case MATCH_ELEMENT_PATH_NAMESPACE:
          g_kdbus_bloom_add_pair(kdbus, bloom, "path-slash-prefix", match->elements[i].value);
          need_bloom = TRUE;
        break;

        case MATCH_ELEMENT_ARG0NAMESPACE:
          g_kdbus_bloom_add_prefixes(kdbus, bloom, "arg0-dot-prefix", match->elements[i].value, '.');
          need_bloom = TRUE;
        break;

        case MATCH_ELEMENT_ARGN:
          sprintf(argument_buf, "arg%d", match->elements->arg);
          g_kdbus_bloom_add_pair(kdbus, bloom, argument_buf, match->elements[i].value);
          need_bloom = TRUE;
        break;

        case MATCH_ELEMENT_ARGNPATH:
          sprintf(argument_buf, "arg%d-slash-prefix", match->elements->arg);
          g_kdbus_bloom_add_prefixes(kdbus, bloom, argument_buf, match->elements[i].value, '/');
          need_bloom = TRUE;
        break;
      }
    }

 if(need_bloom)
    msg_size += KDBUS_ITEM_HEADER_SIZE + kdbus->priv->bloom_size;

  msg = alloca(msg_size);
  if(msg == NULL)
    return FALSE;

  msg->cookie = match->kdbus_cookie;
  msg->size = msg_size;
  item = msg->items;

  if(sender == 0)
    {
      item->type = KDBUS_ITEM_NAME;
      item->size = KDBUS_ITEM_HEADER_SIZE + sender_size;
      memcpy(item->str, string, sender_size);
      item = KDBUS_ITEM_NEXT(item);
    }

  if(src_id != KDBUS_MATCH_ID_ANY)
    {
      item->type = KDBUS_ITEM_ID;
      item->size = KDBUS_ITEM_HEADER_SIZE + sizeof(__u64);
      item->id = src_id;
      item = KDBUS_ITEM_NEXT(item);
    }

  if(need_bloom)
    {
      item->type = KDBUS_ITEM_BLOOM_MASK;
      item->size = KDBUS_ITEM_HEADER_SIZE + kdbus->priv->bloom_size;
      memcpy(item->data, bloom, kdbus->priv->bloom_size);
    }

  if(ioctl(kdbus->priv->fd, KDBUS_CMD_MATCH_ADD, msg))
    {
      g_debug ("[KDBUS] adding match rule failed, cookie %llu, error: %d, %m", (unsigned long long)match->kdbus_cookie, errno);
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno), _("Error when adding match rule, %s"), g_strerror (errno));
      return FALSE;
    }

  g_debug ("[KDBUS] added match rule %llu", (unsigned long long)match->kdbus_cookie);
  return TRUE;
}


/*
 * g_kdbus_remove_match()  
 */
static gboolean
g_kdbus_remove_match (GKdbus   *kdbus,
                      guint64   kdbus_cookie,
                      GError  **error)
{
  struct kdbus_cmd_match cmd;

  cmd.cookie = kdbus_cookie;
  cmd.size = sizeof(struct kdbus_cmd_match);

  if(ioctl(kdbus->priv->fd, KDBUS_CMD_MATCH_REMOVE, &cmd))
    {
      g_debug ("[KDBUS] removing match rule %llu failed, error: %d, %m", (unsigned long long)kdbus_cookie, errno);
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno), _("Error when removing match rule, %s"), g_strerror (errno));
      return FALSE;
    }

  return TRUE;
}

/*
 * kdbus_handle_name_owner_changed()
 */
static GDBusMessage*
kdbus_handle_name_owner_changed (const char  *bus_name,
                                 __u64        old_id,
                                 __u64        new_id,
                                 GKdbus      *kdbus)
{
  GDBusMessage *local_msg = NULL;
  gchar *name = NULL;
  gchar *old = NULL;
  gchar *new = NULL;
  GVariant *g_variant;

  if(bus_name == NULL)
    if(asprintf(&name,":1.%020llu", old_id != 0 ? old_id : new_id) < 0)
      goto out;

  if(old_id != 0)
    if(asprintf(&old,":1.%020llu", old_id) < 0)
      goto out;

  if(new_id != 0)
    if (asprintf(&new,":1.%020llu", new_id) < 0)
      goto out;

  g_variant = g_variant_new ("(sss)",
                              bus_name == NULL ? name : bus_name,
                              old_id != 0 ? old : "",
                              new_id != 0 ? new : "");
  if (g_variant == NULL)
    goto out;

  local_msg = g_kdbus_generate_local_dbus_msg (NULL, G_DBUS_MESSAGE_TYPE_SIGNAL,
                                               G_DBUS_MESSAGE_FLAGS_NO_REPLY_EXPECTED,
                                               g_variant, "NameOwnerChanged", kdbus);
out:
  free(name);
  free(old);
  free(new);

  return local_msg;
}


/*
 * g_kdbus_release_kmsg()
 */
void
g_kdbus_release_kmsg (GKdbus  *kdbus,
                      gsize    size)
{
again:
  if (ioctl(kdbus->priv->fd, KDBUS_CMD_FREE, &kdbus->priv->offset) < 0)
    {
      if(errno == EINTR)
        goto again;

      g_debug ("[KDBUS] KDBUS_CMD_FREE error: %m (%d)", errno);
    }

  if (kdbus->priv->memfd >= 0)
    {
      munmap(kdbus->priv->msg_buffer_ptr, size);
      close(kdbus->priv->memfd);
      kdbus->priv->memfd = -1;
    }
  else if (!address_in_buffer(kdbus->priv->buffer_ptr,
                              kdbus->priv->receive_pool_size,
                              kdbus->priv->msg_buffer_ptr))
    {
      g_free(kdbus->priv->msg_buffer_ptr);
      kdbus->priv->msg_buffer_ptr = NULL;
    }
}


/*
 * g_kdbus_decode_msg()
 */
static int
g_kdbus_decode_msg (GKdbus            *kdbus,
                    struct kdbus_msg  *msg,
                    GError           **error)
{
  GDBusMessage *local_msg = NULL;
  gsize local_msg_blob_size;
  const struct kdbus_item *item;
  int ret_size = 0;
  static gboolean lock = FALSE;

  KDBUS_ITEM_FOREACH(item, msg, items)
  {
    if (item->size < KDBUS_ITEM_HEADER_SIZE)
      {
        g_debug ("[KDBUS] item=%llu (%llu bytes) invalid data record", item->type, item->size);
        g_set_error (error, G_IO_ERROR, g_io_error_from_errno(errno),_("kdbus decode error - invalid data record"));
        ret_size = -1;
        break;
      }

    switch (item->type)
      {
        case KDBUS_ITEM_PAYLOAD_OFF:
          if (!lock)
            {
              kdbus->priv->msg_buffer_ptr = (char *)msg + item->vec.offset;
              lock=TRUE;
            }
          ret_size += item->vec.size;
          g_debug ("[KDBUS] KDBUS_MSG_PAYLOAD: %llu bytes, off=%llu, size=%llu", item->size,
                  (unsigned long long)item->vec.offset, (unsigned long long)item->vec.size);
        break;

        case KDBUS_ITEM_PAYLOAD_MEMFD:
          kdbus->priv->msg_buffer_ptr = mmap(NULL, item->memfd.size, PROT_READ , MAP_SHARED, item->memfd.fd, 0);
          if (kdbus->priv->msg_buffer_ptr == MAP_FAILED)
            {
              g_debug("mmap for KDBUS_MSG_PAYLOAD_MEMFD failed: %m, %d", errno);
              g_set_error (error, G_IO_ERROR, g_io_error_from_errno(errno),_("kdbus decode error - mmap for KDBUS_MSG_PAYLOAD_MEMFD failed"));
              return -1;
            }
          kdbus->priv->memfd = item->memfd.fd;
          ret_size = item->memfd.size;
          g_debug ("[KDBUS] KDBUS_MSG_PAYLOAD_MEMFD: %llu bytes, size=%llu", item->size, (unsigned long long)item->memfd.size);
        break;

        case KDBUS_ITEM_FDS:
          kdbus->priv->num_fds = (item->size - KDBUS_ITEM_HEADER_SIZE) / sizeof(int);
          kdbus->priv->fds = g_malloc0 (sizeof(int) * kdbus->priv->num_fds);
          memcpy(kdbus->priv->fds, item->fds, sizeof(int) * kdbus->priv->num_fds);
        break;

        case KDBUS_ITEM_REPLY_TIMEOUT:
          g_debug ("[KDBUS] KDBUS_MSG_REPLY_TIMEOUT: %llu bytes, cookie=%llu", item->size, msg->cookie_reply);
          local_msg = g_kdbus_generate_local_dbus_msg (NULL, G_DBUS_MESSAGE_TYPE_ERROR,
                                                       G_DBUS_MESSAGE_FLAGS_NO_REPLY_EXPECTED,
                                                       g_variant_new ("(s)","Reply timeout"),
                                                       "org.freedesktop.DBus.Error.NoReply",
                                                       kdbus);
          g_dbus_message_set_reply_serial(local_msg, msg->cookie_reply);
          kdbus->priv->msg_buffer_ptr = (char*)g_dbus_message_to_blob (local_msg, &local_msg_blob_size, 0, error);
          if(kdbus->priv->msg_buffer_ptr == NULL)
            ret_size = -1;
          else
            ret_size = local_msg_blob_size;
        break;

        case KDBUS_ITEM_REPLY_DEAD:
          g_debug ("[KDBUS] KDBUS_MSG_REPLY_DEAD: %llu bytes, cookie=%llu", item->size, msg->cookie_reply);
          local_msg = g_kdbus_generate_local_dbus_msg (NULL, G_DBUS_MESSAGE_TYPE_ERROR,
                                                       G_DBUS_MESSAGE_FLAGS_NO_REPLY_EXPECTED,
                                                       g_variant_new ("(s)","Name has no owner"),
                                                       "org.freedesktop.DBus.Error.NameHasNoOwner",
                                                       kdbus);
          g_dbus_message_set_reply_serial(local_msg, msg->cookie_reply);
          kdbus->priv->msg_buffer_ptr = (char*)g_dbus_message_to_blob (local_msg, &local_msg_blob_size, 0, error);
          if(kdbus->priv->msg_buffer_ptr == NULL)
            ret_size = -1;
          else
            ret_size = local_msg_blob_size;
        break;

        case KDBUS_ITEM_NAME_ADD:
        case KDBUS_ITEM_NAME_REMOVE:
        case KDBUS_ITEM_NAME_CHANGE:
          {
            gsize first_size = 0;
            gchar *first_buf = NULL;

            if(item->name_change.new.id == kdbus->priv->unique_id)
              {
                g_debug ("[KDBUS] generating NameAcquired for %s", item->name_change.name);

                local_msg = g_kdbus_generate_local_dbus_msg (NULL, G_DBUS_MESSAGE_TYPE_SIGNAL,
                                                           G_DBUS_MESSAGE_FLAGS_NO_REPLY_EXPECTED,
                                                           g_variant_new ("(s)",item->name_change.name),
                                                           "NameAcquired", kdbus);
                first_buf = (char*)g_dbus_message_to_blob (local_msg, &first_size, 0, error);
                if(first_buf == NULL)
                  {
                    ret_size = -1;
                    break;
                  }
              }
            else if(item->name_change.old.id == kdbus->priv->unique_id)
              {
                g_debug ("[KDBUS] generating NameLost for %s", item->name_change.name);

                local_msg = g_kdbus_generate_local_dbus_msg (NULL, G_DBUS_MESSAGE_TYPE_SIGNAL,
                                                           G_DBUS_MESSAGE_FLAGS_NO_REPLY_EXPECTED,
                                                           g_variant_new ("(s)",item->name_change.name),
                                                           "NameLost", kdbus);
                first_buf = (char*)g_dbus_message_to_blob (local_msg, &first_size, 0, error);
                if(first_buf == NULL)
                  {
                    ret_size = -1;
                    break;
                  }
              }

            if(item->name_change.new.flags & KDBUS_NAME_ACTIVATOR)
              local_msg = kdbus_handle_name_owner_changed(item->name_change.name,
                                                          item->name_change.old.id, 0,
                                                          kdbus);
            else if(item->name_change.old.flags & KDBUS_NAME_ACTIVATOR)
              local_msg = kdbus_handle_name_owner_changed(item->name_change.name, 0,
                                                          item->name_change.new.id,
                                                          kdbus);
            else
              local_msg = kdbus_handle_name_owner_changed(item->name_change.name,
                                                          item->name_change.old.id,
                                                          item->name_change.new.id,
                                                          kdbus);
            if (local_msg == NULL)
              {
                g_set_error (error, G_IO_ERROR, g_io_error_from_errno(errno),_("kdbus decode error - generating DBus message failed"));
                if(first_size > 0)
                  g_free(first_buf);
                ret_size = -1;
                break;
              }

            kdbus->priv->msg_buffer_ptr = (char*)g_dbus_message_to_blob (local_msg, (gsize*)&ret_size, 0, error);
            if(kdbus->priv->msg_buffer_ptr == NULL)
              {
                if(first_size > 0)
                  g_free(first_buf);
                ret_size = -1;
              }

            if(first_size > 0)
              {
                 gchar *second_buf;

                 second_buf = kdbus->priv->msg_buffer_ptr;
                 kdbus->priv->msg_buffer_ptr = g_malloc(first_size + ret_size);
                 if(kdbus->priv->msg_buffer_ptr == NULL)
                   {
                     g_free(first_buf);
                     g_free(second_buf);
                     g_set_error (error, G_IO_ERROR, g_io_error_from_errno(errno),_("NamwOwnerChange generating error"));
                     ret_size = -1;
                   }

                 memcpy(kdbus->priv->msg_buffer_ptr, first_buf, first_size);
                 memcpy(kdbus->priv->msg_buffer_ptr + first_size, second_buf, ret_size);
                 g_free(first_buf);
                 g_free(second_buf);
                 ret_size += first_size;
              }
          }
          break;

        case KDBUS_ITEM_ID_ADD:
        case KDBUS_ITEM_ID_REMOVE:

          g_debug ("[KDBUS] +KDBUS_ITEM_ID_* (%llu bytes) id=%llu flags=%llu",
                    (unsigned long long) item->size,
                    (unsigned long long) item->id_change.id,
                    (unsigned long long) item->id_change.flags);

          if(item->id_change.flags & KDBUS_HELLO_ACTIVATOR)
            {
              ret_size = 0;
              kdbus->priv->msg_buffer_ptr = NULL;
              break;
            }
          else
            local_msg = kdbus_handle_name_owner_changed(NULL,
                             item->type == KDBUS_ITEM_ID_ADD ? 0 : item->id_change.id,
                             item->type == KDBUS_ITEM_ID_ADD ? item->id_change.id : 0,
                             kdbus);

          if (local_msg == NULL)
            {
              g_set_error (error, G_IO_ERROR, g_io_error_from_errno(errno),_("kdbus decode error - generating DBus message failed"));
              ret_size = -1;
              break;
            }

          kdbus->priv->msg_buffer_ptr = (char*)g_dbus_message_to_blob (local_msg, (gsize*) &ret_size, 0, error);
          if(kdbus->priv->msg_buffer_ptr == NULL)
            ret_size = -1;

        break;

        default:
          g_debug ("[KDBUS] unknown item type");
        break;
      }
  }

  if(local_msg)
    g_object_unref (local_msg);
  lock = FALSE;
  return ret_size;
}


/*
 * g_kdbus_receive()
 */
gssize
g_kdbus_receive (GKdbus        *kdbus,
                 GCancellable  *cancellable,
                 GError       **error)
{
  struct kdbus_msg *msg;
  struct kdbus_cmd_recv recv = {};

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return -1;

again:
  if (ioctl(kdbus->priv->fd, KDBUS_CMD_MSG_RECV, &recv) < 0)
    {
      if(errno == EINTR)
        goto again;

      g_debug ("[KDBUS] error receiving message: %d (%m)", errno);
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno(errno),_("Error receiving message - KDBUS_CMD_MSG_RECV error"));
      return -1;
    }

  kdbus->priv->offset = recv.offset;
  msg = (struct kdbus_msg *)((char*)kdbus->priv->buffer_ptr + kdbus->priv->offset);
  return (g_kdbus_decode_msg(kdbus, msg, error));
}


/*
 * g_kdbus_send_message()
 */
gssize
g_kdbus_send_message (GDBusWorker    *worker,
                      GKdbus         *kdbus,
                      GDBusMessage   *dbus_msg,
                      gchar          *blob,
                      gsize           blob_size,
                      GCancellable   *cancellable,
                      GError        **error)
{
  struct kdbus_msg  *kmsg;
  struct kdbus_item *item;
  const gchar *name;
  guint64 kmsg_size, dst_id;
  gint memfd;
  GUnixFDList *fd_list;
  GString *error_name;

  fd_list = NULL;
  error_name = NULL;
  kmsg_size = 0;
  memfd = -1;
  dst_id = KDBUS_DST_ID_BROADCAST;

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return -1;

  if (kdbus->priv->registered == FALSE)
    {
      if (!g_kdbus_register(kdbus))
        {
          g_set_error (error, G_IO_ERROR, g_io_error_from_errno(errno),_("Error sending message - registering failed"));
          return -1;
        }

      if (g_strcmp0(g_dbus_message_get_member(dbus_msg), "Hello") == 0)
        {
          GDBusMessage *reply;

          g_debug ("[KDBUS] captured \"Hello\" message!");
          reply = g_kdbus_generate_local_dbus_msg (dbus_msg,
                                                   G_DBUS_MESSAGE_TYPE_METHOD_RETURN,
                                                   G_DBUS_MESSAGE_FLAGS_NO_REPLY_EXPECTED,
                                                   g_variant_new ("(s)",kdbus->priv->my_DBus_uniqe_name),
                                                   NULL, kdbus);
          if(reply == NULL)
            {
              g_set_error (error, G_IO_ERROR, g_io_error_from_errno(errno),_("Error sending message - %m"));
              return -1;
            }

          _g_dbus_worker_queue_or_deliver_received_message (worker, reply);
          return blob_size;
        }
    }

#ifdef POLICY_IN_LIB
  if (!policy_check_can_send(kdbus->priv->policy, dbus_msg))
    {
      if(!send_local_cant_send_error(worker, kdbus, dbus_msg))
        {
          g_set_error (error, G_IO_ERROR, g_io_error_from_errno(errno),_("Error sending message - %m"));
          return -1;
        }
      return blob_size;
    }
#endif

#if 1
  if (!g_strcmp0(g_dbus_message_get_destination(dbus_msg), "org.freedesktop.DBus"))
    {
      GDBusMessage *reply = NULL;

      if (g_strcmp0(g_dbus_message_get_member(dbus_msg), "RequestName") == 0)
        {
          GVariant *body = NULL;
          const gchar *name = NULL;
          guint32 flags;

          body = g_dbus_message_get_body (dbus_msg);

          if (g_variant_is_of_type (body, G_VARIANT_TYPE("(su)")))
            {
              g_variant_get (body, "(&su)", &name, &flags);

              g_debug ("[KDBUS] captured \"RequestName\" call for name: %s!", name);
              g_kdbus_request_name(kdbus, name, &flags, error);
              g_debug ("[KDBUS] request name - return flag: 0x%x", flags);
            }
          else
            {
              g_debug ("[KDBUS] call to 'RequestName' has wrong args (expected su)");
              g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                           "Call to 'RequestName' has wrong args (expected su)");
            }

          if(*error != NULL)
            {
              reply = g_kdbus_generate_local_dbus_msg (dbus_msg,
                                                     G_DBUS_MESSAGE_TYPE_ERROR,
                                                     G_DBUS_MESSAGE_FLAGS_NO_REPLY_EXPECTED,
                                                     g_variant_new ("(s)", (*error)->message),
                                                     g_dbus_error_get_remote_error(*error), kdbus);
              g_error_free(*error);
            }
          else
            {
              reply = g_kdbus_generate_local_dbus_msg (dbus_msg,
                                                     G_DBUS_MESSAGE_TYPE_METHOD_RETURN,
                                                     G_DBUS_MESSAGE_FLAGS_NO_REPLY_EXPECTED,
                                                     g_variant_new ("(u)", flags), NULL, kdbus);
            }

          if(reply == NULL)
            {
              g_set_error (error, G_IO_ERROR, g_io_error_from_errno(errno),_("Error sending message - %m"));
              return -1;
            }
        }
      else if (g_strcmp0(g_dbus_message_get_member(dbus_msg), "ReleaseName") == 0)
        {
          GVariant *body = NULL;
          const gchar *name = NULL;
          guint32 result = 4;

          body = g_dbus_message_get_body (dbus_msg);
          if (g_variant_is_of_type (body, G_VARIANT_TYPE("(s)")))
            {
              g_variant_get (body, "(&s)", &name);

              g_debug ("[KDBUS] captured \"ReleaseName\" call for name: %s!", name);
              result = g_kdbus_release_name (kdbus, name, error);
              g_debug ("[KDBUS] release name result: %d", result);
            }
          else
            {
              g_debug ("[KDBUS] call to 'ReleaseName' has wrong args (expected s)");
              g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                           "Call to 'ReleaseName' has wrong args (expected s)");
            }

          if(*error != NULL)
            {
              reply = g_kdbus_generate_local_dbus_msg (dbus_msg,
                                                     G_DBUS_MESSAGE_TYPE_ERROR,
                                                     G_DBUS_MESSAGE_FLAGS_NO_REPLY_EXPECTED,
                                                     g_variant_new ("(s)", (*error)->message),
                                                     g_dbus_error_get_remote_error(*error), kdbus);
              g_error_free(*error);
            }
          else
            {
              reply = g_kdbus_generate_local_dbus_msg (dbus_msg,
                                                     G_DBUS_MESSAGE_TYPE_METHOD_RETURN,
                                                     G_DBUS_MESSAGE_FLAGS_NO_REPLY_EXPECTED,
                                                     g_variant_new ("(u)", result), NULL, kdbus);
            }

          if(reply == NULL)
            {
              g_set_error (error, G_IO_ERROR, g_io_error_from_errno(errno),_("Error sending message - %m"));
              return -1;
            }
        }
      else if (g_strcmp0(g_dbus_message_get_member(dbus_msg), "AddMatch") == 0)
        {
          Match *match;
          const gchar *rule;

          if (g_variant_is_of_type (g_dbus_message_get_body (dbus_msg), G_VARIANT_TYPE("(s)")))
            {
              rule = message_get_argN(dbus_msg, 0, FALSE);
              if (rule != NULL)
                {
                  g_debug ("[KDBUS] captured \"AddMatch\" message, with rule: %s!", rule);
                  match = match_new (rule);
                  if (match == NULL)
                    {
                      g_debug ("[KDBUS] creating match rule from the given string failed");
                      g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_MATCH_RULE_INVALID,
                                   "Creating match rule from the given string failed.");
                    }
                  else
                    {
                      if(g_kdbus_addmatch(kdbus, match, error))
                        kdbus->priv->matches = g_list_prepend (kdbus->priv->matches, match);
                    }
                }
              else
                {
                  g_debug ("[KDBUS] creating match rule from the given string failed");
                  g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_MATCH_RULE_INVALID,
                               "Creating match rule from the given string failed.");

                }
            }
          else
            {
              g_debug ("[KDBUS] call to 'AddMatch' has wrong args (expected s)");
              g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "Call to 'AddMatch' has wrong args (expected s)");
            }

          if(*error != NULL)
            {
              reply = g_kdbus_generate_local_dbus_msg (dbus_msg,
                                                     G_DBUS_MESSAGE_TYPE_ERROR,
                                                     G_DBUS_MESSAGE_FLAGS_NO_REPLY_EXPECTED,
                                                     g_variant_new ("(s)", (*error)->message),
                                                     g_dbus_error_get_remote_error(*error), kdbus);
              g_error_free(*error);
            }
          else
            {
              reply = g_kdbus_generate_local_dbus_msg (dbus_msg,
                                                     G_DBUS_MESSAGE_TYPE_METHOD_RETURN,
                                                     G_DBUS_MESSAGE_FLAGS_NO_REPLY_EXPECTED,
                                                     NULL, NULL, kdbus);
            }

          if(reply == NULL)
            {
              g_set_error (error, G_IO_ERROR, g_io_error_from_errno(errno),_("Error sending message - %m"));
              return -1;
            }
        }
      else if(g_strcmp0(g_dbus_message_get_member(dbus_msg), "RemoveMatch") == 0)
        {
          Match *match_from_msg, *match_in_lib;
          GList *l;
          const gchar *rule;

          if (g_variant_is_of_type (g_dbus_message_get_body (dbus_msg), G_VARIANT_TYPE("(s)")))
            {
              rule = message_get_argN(dbus_msg, 0, FALSE);
              if (rule != NULL)
                {
                  match_from_msg = match_new (rule);
                  if (match_from_msg == NULL)
                    {
                      g_debug ("[KDBUS] creating match rule from the given string failed");
                      g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_MATCH_RULE_INVALID,
                                   "Creating match rule from the given string failed.");
                    }
                  else
                    {
                      for (l = kdbus->priv->matches; l != NULL; l = l->next)
                        {
                          match_in_lib = l->data;
                          if (match_equal (match_from_msg, match_in_lib))
                            {
                              if(g_kdbus_remove_match(kdbus, match_in_lib->kdbus_cookie, error))
                                {
                                  match_free (match_in_lib);
                                  kdbus->priv->matches = g_list_delete_link (kdbus->priv->matches, l);
                                }
                              break;
                            }
                        }

                      if (l == NULL)
                        {
                          g_debug ("[KDBUS] the given match rule wasn't found and can't be removed");
                          g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_MATCH_RULE_NOT_FOUND,
                                       "The given match rule wasn't found and can't be removed");
                        }
                      match_free (match_from_msg);
                    }
                }
              else
                {
                  g_debug ("[KDBUS] creating match rule from the given string failed");
                  g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_MATCH_RULE_INVALID,
                               "Creating match rule from the given string failed.");

                }
            }
          else
            {
              g_debug ("[KDBUS] call to 'RemoveMatch' has wrong args (expected s)");
              g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "Call to 'RemoveMatch' has wrong args (expected s)");
            }

          if(*error != NULL)
            {
              reply = g_kdbus_generate_local_dbus_msg (dbus_msg,
                                                     G_DBUS_MESSAGE_TYPE_ERROR,
                                                     G_DBUS_MESSAGE_FLAGS_NO_REPLY_EXPECTED,
                                                     g_variant_new ("(s)", (*error)->message),
                                                     g_dbus_error_get_remote_error(*error), kdbus);
              g_error_free(*error);
            }
          else
            {
              reply = g_kdbus_generate_local_dbus_msg (dbus_msg,
                                                     G_DBUS_MESSAGE_TYPE_METHOD_RETURN,
                                                     G_DBUS_MESSAGE_FLAGS_NO_REPLY_EXPECTED,
                                                     NULL, NULL, kdbus);
            }

          if(reply == NULL)
            {
              g_set_error (error, G_IO_ERROR, g_io_error_from_errno(errno),_("Error sending message - %m"));
              return -1;
            }
        }

      if(reply)
        {
          _g_dbus_worker_queue_or_deliver_received_message (worker, reply);
          return blob_size;
        }
    }
#endif

  /*
   * check destination
   */
  if ((name = g_dbus_message_get_destination(dbus_msg)))
    {
      dst_id = KDBUS_DST_ID_NAME;
      if ((name[0] == ':') && (name[1] == '1') && (name[2] == '.'))
        {
          dst_id = strtoull(&name[3], NULL, 10);
          name=NULL;
        }
    }

  /*
   * check memfd
   */
  if ((blob_size > MEMFD_SIZE_THRESHOLD) && (dst_id != KDBUS_DST_ID_BROADCAST))
    memfd = g_kdbus_acquire_memfd(kdbus, blob_size);

  /*
   * check and set message size
   */
  kmsg_size = sizeof(struct kdbus_msg);

  if (memfd >= 0)
    kmsg_size += KDBUS_ITEM_SIZE(sizeof(struct kdbus_memfd));
  else
    {
      signed long long data_size = blob_size;

      while(data_size > KDBUS_MSG_MAX_PAYLOAD_VEC_SIZE)
        {
          kmsg_size += KDBUS_ITEM_SIZE(sizeof(struct kdbus_vec));
          data_size -= KDBUS_MSG_MAX_PAYLOAD_VEC_SIZE;
        }
      if(data_size > 0)
        kmsg_size += KDBUS_ITEM_SIZE(sizeof(struct kdbus_vec));
    }

  fd_list = g_dbus_message_get_unix_fd_list(dbus_msg);
  if (fd_list != NULL && g_unix_fd_list_get_length (fd_list) > 0)
    kmsg_size += KDBUS_ITEM_HEADER_SIZE + sizeof(int) * g_unix_fd_list_get_length(fd_list);

  if (name)
    kmsg_size += KDBUS_ITEM_SIZE(strlen(name) + 1);
  else if (dst_id == KDBUS_DST_ID_BROADCAST)
    kmsg_size += KDBUS_ITEM_SIZE(sizeof(struct kdbus_bloom_filter) + kdbus->priv->bloom_size);

  kmsg = malloc(kmsg_size);
  if (!kmsg)
    {
      g_debug ("[KDBUS] can't allocate memory for new message to send");
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno(errno),_("Error sending message - kmsg malloc error"));
      return -1;
    }

  /*
   * set message header
   */
  memset(kmsg, 0, kmsg_size);
  kmsg->size = kmsg_size;
  kmsg->payload_type = KDBUS_PAYLOAD_DBUS;
  kmsg->dst_id = name ? 0 : dst_id;
  kmsg->src_id = kdbus->priv->unique_id;
  kmsg->cookie = g_dbus_message_get_serial(dbus_msg);

  /*
   * set message flags
   */
  if (g_dbus_message_get_flags (dbus_msg) & G_DBUS_MESSAGE_FLAGS_NO_AUTO_START)
    kmsg->flags |= KDBUS_MSG_FLAGS_NO_AUTO_START;

  if(!(g_dbus_message_get_flags (dbus_msg) & G_DBUS_MESSAGE_FLAGS_NO_REPLY_EXPECTED) && (dst_id != KDBUS_DST_ID_BROADCAST))
    {
      kmsg->flags |= KDBUS_MSG_FLAGS_EXPECT_REPLY;
      kmsg->timeout_ns = KDBUS_REPLY_TIMEOUT;
    }
  else
    kmsg->cookie_reply = g_dbus_message_get_reply_serial(dbus_msg);

  g_debug ("[KDBUS] sending message; destination:%s, blob size: %d, serial: %llu", name, (gint)blob_size, kmsg->cookie);

  /* build message contents */
  item = kmsg->items;

  if (memfd >= 0)
    {
      guint64 counter = blob_size;
      gint64 written;
      gchar* data = blob;

      while (counter)
        {
          written = write(memfd, data, counter);
          if (written < 0)
            {
              g_debug ("[KDBUS] writing to memfd failed: (%d) %m", errno);
              g_set_error (error, G_IO_ERROR, g_io_error_from_errno(errno),_("Error sending message - write memfd error"));
              blob_size = -1;
              goto out;
            }
          counter -= written;
          data += written;
        }

      /* seal data - kdbus module requires it */
      if(ioctl(memfd, KDBUS_CMD_MEMFD_SEAL_SET, 1) < 0)
        {
          g_debug ("[KDBUS] KDBUS_CMD_MEMFD_SEAL_SET 1 failed: %m (%d)", errno);
          g_set_error (error, G_IO_ERROR, g_io_error_from_errno(errno),_("Error sending message - memfd seal set error"));
          blob_size = -1;
          goto out;
        }

      /* append memfd */
      g_kdbus_append_payload_memfd (&item, memfd, blob_size);

    }
  else
    {
      signed long long data_size = blob_size;

      while(data_size > KDBUS_MSG_MAX_PAYLOAD_VEC_SIZE)
        {
          g_kdbus_append_payload_vec (&item, blob, KDBUS_MSG_MAX_PAYLOAD_VEC_SIZE);
          blob += KDBUS_MSG_MAX_PAYLOAD_VEC_SIZE;
          data_size -= KDBUS_MSG_MAX_PAYLOAD_VEC_SIZE;
        }

      if(data_size > 0)
        g_kdbus_append_payload_vec (&item, blob, data_size);
    }

  if (name)
    g_kdbus_append_destination (&item, name, strlen(name));
  else if (dst_id == KDBUS_DST_ID_BROADCAST)
    {
      struct kdbus_bloom_filter *bloom_filter;
      bloom_filter = g_kdbus_append_bloom (&item, kdbus->priv->bloom_size);
      g_kdbus_setup_bloom (kdbus, dbus_msg, bloom_filter);
    }

  if (fd_list != NULL && g_unix_fd_list_get_length (fd_list) > 0)
    g_kdbus_append_fds (&item, fd_list);

  error_name = g_string_new (NULL);
again:
  if (ioctl(kdbus->priv->fd, KDBUS_CMD_MSG_SEND, kmsg))
    {
      GDBusMessage *reply;

      if(errno == EINTR)
        {
          goto again;
        }
      else if (errno == ENXIO)
        {
          g_string_printf (error_name, "Name %s does not exist", g_dbus_message_get_destination(dbus_msg));
          reply = g_kdbus_generate_local_dbus_msg (dbus_msg,
                                                   G_DBUS_MESSAGE_TYPE_ERROR,
                                                   G_DBUS_MESSAGE_FLAGS_NO_REPLY_EXPECTED,
                                                   g_variant_new ("(s)",error_name->str),
                                                   "org.freedesktop.DBus.Error.ServiceUnknown", kdbus);
          _g_dbus_worker_queue_or_deliver_received_message (worker, reply);
          goto out;
        }
      else if ((errno == ESRCH) || (errno == EADDRNOTAVAIL))
        {
          if (kmsg->flags & KDBUS_MSG_FLAGS_NO_AUTO_START)
            {
              g_string_printf (error_name, "Name %s does not exist", g_dbus_message_get_destination(dbus_msg));
              reply = g_kdbus_generate_local_dbus_msg (dbus_msg,
                                                       G_DBUS_MESSAGE_TYPE_ERROR,
                                                       G_DBUS_MESSAGE_FLAGS_NO_REPLY_EXPECTED,
                                                       g_variant_new ("(s)",error_name->str),
                                                       "org.freedesktop.DBus.Error.ServiceUnknown", kdbus);
              _g_dbus_worker_queue_or_deliver_received_message (worker, reply);
              goto out;
            }
          else
            {
              g_string_printf (error_name, "The name %s was not provided by any .service files", g_dbus_message_get_destination(dbus_msg));
              reply = g_kdbus_generate_local_dbus_msg (dbus_msg,
                                                       G_DBUS_MESSAGE_TYPE_ERROR,
                                                       G_DBUS_MESSAGE_FLAGS_NO_REPLY_EXPECTED,
                                                       g_variant_new ("(s)",error_name->str),
                                                       "org.freedesktop.DBus.Error.ServiceUnknown", kdbus);
              _g_dbus_worker_queue_or_deliver_received_message (worker, reply);
              goto out;
            }
        }

      g_debug ("[KDBUS] kdbus ioctl error while sending kdbus message:%d (%m)", errno);
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno(errno), _("Error sending message - KDBUS_CMD_MSG_SEND error"));
      blob_size = -1;
      goto out;
    }

  g_debug ("[KDBUS] ioctl(CMD_MSG_SEND) sent successfully");

out:
  if (error_name != NULL)
    g_string_free(error_name, TRUE);
  if (memfd >= 0)
    close(memfd);
  free(kmsg);
  return blob_size;
}


#ifdef POLICY_IN_LIB


/*
 * _g_kdbus_get_policy()
 */
PBusClientPolicy*
_g_kdbus_get_policy (GKdbus  *kdbus)
{
  return kdbus->priv->policy;
}


/*
 * policy_get_libdbuspolicy_msg_type()
 */
static gint
policy_get_libdbuspolicy_msg_type (GDBusMessage *message)
{
  gint libdbuspolicy_msg_type = 0;
  gint dbus_msg_type = 0;

  dbus_msg_type = g_dbus_message_get_message_type (message);

  if (G_DBUS_MESSAGE_TYPE_METHOD_CALL == dbus_msg_type)
    libdbuspolicy_msg_type = 1;
    else if (G_DBUS_MESSAGE_TYPE_METHOD_RETURN == dbus_msg_type)
      libdbuspolicy_msg_type = 2;
    else if (G_DBUS_MESSAGE_TYPE_ERROR == dbus_msg_type)
      libdbuspolicy_msg_type = 4;
    else if (G_DBUS_MESSAGE_TYPE_SIGNAL == dbus_msg_type)
      libdbuspolicy_msg_type = 3;
    else
      g_debug ("[KDBUS] messages should have type!");

  return libdbuspolicy_msg_type;
}


/*
 * policy_get_libdbuspolicy_msg_req_reply()
 */
static int
policy_get_libdbuspolicy_msg_req_reply (GDBusMessage *message)
{
  if (g_dbus_message_get_reply_serial(message) == 0)
    return NO_REQUESTED_REPLY;
  return REQUESTED_REPLY;
}


/*
 * policy_check_can_send()
 */
static gboolean
policy_check_can_send (void          *policy,
                       GDBusMessage  *message)
{
  if (dbus_policy_check_can_send(policy,
                                 policy_get_libdbuspolicy_msg_type(message),       /* message type    */
                                 g_dbus_message_get_destination (message),         /* destination     */
                                 g_dbus_message_get_path (message),                /* path            */
                                 g_dbus_message_get_interface (message),           /* interface       */
                                 g_dbus_message_get_member (message),              /* member          */
                                 g_dbus_message_get_error_name (message),          /* error name      */
                                 g_dbus_message_get_reply_serial (message),        /* reply serial    */
                                 policy_get_libdbuspolicy_msg_req_reply(message))) /* requested reply */
    {
      g_debug ("[KDBUS] policy 'can send' checked. This message is ok");
      return TRUE;
    }
  else
    {
      g_debug ("[KDBUS] policy 'can send' checked. I can't send this msg");
      return FALSE;
    }
}


/*
 * policy_check_can_receive()
 */
gboolean
policy_check_can_receive (PBusClientPolicy  *policy,
                          GDBusMessage      *message)
{
  if (dbus_policy_check_can_recv (policy,
                                  policy_get_libdbuspolicy_msg_type(message),       /* method call     */
                                  g_dbus_message_get_sender (message),              /* sender(!)       */
                                  g_dbus_message_get_path (message),                /* path            */
                                  g_dbus_message_get_interface (message),           /* interface       */
                                  g_dbus_message_get_member (message),              /* member          */
                                  g_dbus_message_get_error_name (message),          /* error name      */
                                  g_dbus_message_get_reply_serial (message),        /* reply serial    */
                                  policy_get_libdbuspolicy_msg_req_reply(message))) /* requested reply */
    {
      g_debug ("[KDBUS] policy 'can receive' checked. This message can pass");
      return TRUE;
    }
  else
    {
      g_debug ("[KDBUS] policy 'can receive' checked. I can't receive this message");
      return FALSE;
    }
}


static inline const gchar *
nonnull (const gchar *maybe_null,
         const gchar *if_null)
{
  return (maybe_null ? maybe_null : if_null);
}


/*
 * prepare_error_msg_text()
 * copied from dbus-transport-kdbus.c, which was previously
 * based on complain_about_message(), but with some differences
 */
static gchar *
prepare_error_msg_text (const gchar   *complaint,
                        gint           matched_rules,
                        GDBusMessage  *message,
                        const gchar   *sender_name,
                        gboolean       requested_reply)
{
  gchar *err_msg_text;
  const gchar *sender_loginfo;
  const gchar *proposed_recipient_loginfo;

  /* TODO: can pregenerate for local or ask kdbus for remote data */
  sender_loginfo = "(unset)";

  /* TODO: can pregenerate for local or ask kdbus for remote data */
  proposed_recipient_loginfo = "(unset)";

  if (asprintf(&err_msg_text,
      "%s, %d matched rules; type=\"%s\", sender=\"%s\" (%s) "
      "interface=\"%s\" member=\"%s\" error name=\"%s\" "
      "requested_reply=\"%d\" destination=\"%s\" (%s)",
      complaint,
      matched_rules,
      _g_dbus_enum_to_string (G_TYPE_DBUS_MESSAGE_TYPE, (g_dbus_message_get_message_type (message))),
      sender_name,
      sender_loginfo,
      nonnull (g_dbus_message_get_interface (message), "(unset)"),
      nonnull (g_dbus_message_get_member (message), "(unset)"),
      nonnull (g_dbus_message_get_error_name (message), "(unset)"),
      requested_reply,
      nonnull (g_dbus_message_get_destination (message), DBUS_SERVICE_DBUS),
      proposed_recipient_loginfo) > 0)
    {
      return err_msg_text;
    }
  else
    {
      return NULL;
    }
}


/*
 * send_local_cant_send_error()
 */
static gboolean
send_local_cant_send_error (GDBusWorker   *worker,
                            GKdbus        *kdbus,
                            GDBusMessage  *dbus_msg)
{
  if (!(g_dbus_message_get_error_name(dbus_msg) &&
      strcmp(g_dbus_message_get_error_name(dbus_msg), DBUS_ERROR_ACCESS_DENIED) == 0))
    /*
     * don't generate local error if msg is local error from checking policy for receiving
     * (for now it is only case of generating DBUS_ERROR_ACCESS_DENIED)
     */
    {
      GDBusMessage *reply;
      char *err_msg_text;
      gboolean requested_reply = FALSE;

      if (g_dbus_message_get_reply_serial (dbus_msg) != 0)
        requested_reply = TRUE;

      err_msg_text = prepare_error_msg_text("Rejected send message",
          -1  /* TODO: insert real value from libdbuspolicy (patch is need) */,
          dbus_msg, kdbus->priv->my_DBus_uniqe_name, requested_reply);
      if (!err_msg_text)
        return FALSE;

      reply = g_kdbus_generate_local_dbus_msg (dbus_msg,
                                               G_DBUS_MESSAGE_TYPE_ERROR,
                                               G_DBUS_MESSAGE_FLAGS_NO_REPLY_EXPECTED,
                                               g_variant_new ("(s)", err_msg_text),
                                               DBUS_ERROR_ACCESS_DENIED, kdbus);
      free (err_msg_text);
      if(reply == NULL)
        return FALSE;

      _g_dbus_worker_queue_or_deliver_received_message (worker, reply);
    }
  return TRUE;
}


/*
 * send_cant_recv_error()
 */
void
send_cant_recv_error (GDBusConnection  *connection,
                      GDBusMessage     *dbus_msg,
                      GKdbus           *kdbus)
{
  GDBusMessage *reply;
  char *err_msg_text;
  gboolean requested_reply = FALSE;
  GError *error = NULL;

  if (g_dbus_message_get_reply_serial (dbus_msg) != 0)
    requested_reply = TRUE;

  err_msg_text = prepare_error_msg_text("Rejected receive message",
      -1 /* TODO: insert real value from libdbuspolicy (patch is need) */,
      dbus_msg, g_dbus_message_get_sender(dbus_msg), requested_reply);
  if (!err_msg_text)
    return;

  reply = g_kdbus_generate_local_dbus_msg (dbus_msg,
                                           G_DBUS_MESSAGE_TYPE_ERROR,
                                           G_DBUS_MESSAGE_FLAGS_NO_REPLY_EXPECTED,
                                           g_variant_new ("(s)", err_msg_text),
                                           DBUS_ERROR_ACCESS_DENIED, kdbus);
  free (err_msg_text);
  if(reply == NULL)
    return;

  g_dbus_message_set_sender(reply, DBUS_SERVICE_DBUS);
  g_dbus_connection_send_message(connection, reply, G_DBUS_SEND_MESSAGE_FLAGS_NONE, NULL, &error);

  if(error)
    g_error_free(error);
}
#endif
