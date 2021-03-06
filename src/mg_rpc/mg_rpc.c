/*
 * Copyright (c) 2014-2018 Cesanta Software Limited
 * All rights reserved
 *
 * Licensed under the Apache License, Version 2.0 (the ""License"");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an ""AS IS"" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdlib.h>
#include <string.h>

#include "mg_rpc.h"
#include "mg_rpc_channel.h"
#include "mg_rpc_channel_ws.h"

#include "common/cs_dbg.h"
#include "common/json_utils.h"
#include "common/mbuf.h"

#include "mongoose.h"

#include "mgos_mongoose.h"
#include "mgos_sys_config.h"

struct mg_rpc {
  struct mg_rpc_cfg *cfg;
  int64_t next_id;
  int queue_len;
  struct mbuf local_ids;

  mg_prehandler_cb_t prehandler;
  void *prehandler_arg;

  SLIST_HEAD(handlers, mg_rpc_handler_info) handlers;
  SLIST_HEAD(channels, mg_rpc_channel_info_internal) channels;
  SLIST_HEAD(requests, mg_rpc_sent_request_info) requests;
  SLIST_HEAD(observers, mg_rpc_observer_info) observers;
  STAILQ_HEAD(queue, mg_rpc_queue_entry) queue;
};

struct mg_rpc_handler_info {
  const char *method;
  const char *args_fmt;
  mg_handler_cb_t cb;
  void *cb_arg;
  SLIST_ENTRY(mg_rpc_handler_info) handlers;
};

struct mg_rpc_channel_info_internal {
  struct mg_str dst;
  struct mg_rpc_channel *ch;
  unsigned int is_open : 1;
  unsigned int is_busy : 1;
  SLIST_ENTRY(mg_rpc_channel_info_internal) channels;
};

struct mg_rpc_sent_request_info {
  int64_t id;
  mg_result_cb_t cb;
  void *cb_arg;
  SLIST_ENTRY(mg_rpc_sent_request_info) requests;
};

struct mg_rpc_queue_entry {
  struct mg_str dst;
  struct mg_str frame;
  /*
   * If this item has been assigned to a particular channel, use it.
   * Otherwise perform lookup by dst.
   */
  struct mg_rpc_channel_info_internal *ci;
  STAILQ_ENTRY(mg_rpc_queue_entry) queue;
};

struct mg_rpc_observer_info {
  mg_observer_cb_t cb;
  void *cb_arg;
  SLIST_ENTRY(mg_rpc_observer_info) observers;
};

static int64_t mg_rpc_get_id(struct mg_rpc *c) {
  c->next_id += rand();
  return c->next_id;
}

static void mg_rpc_call_observers(struct mg_rpc *c, enum mg_rpc_event ev,
                                  void *ev_arg) {
  struct mg_rpc_observer_info *oi, *oit;
  SLIST_FOREACH_SAFE(oi, &c->observers, observers, oit) {
    oi->cb(c, oi->cb_arg, ev, ev_arg);
  }
}

static struct mg_rpc_channel_info_internal *mg_rpc_get_channel_info_internal(
    struct mg_rpc *c, const struct mg_rpc_channel *ch) {
  struct mg_rpc_channel_info_internal *ci;
  if (c == NULL) return NULL;
  SLIST_FOREACH(ci, &c->channels, channels) {
    if (ci->ch == ch) return ci;
  }
  return NULL;
}

static struct mg_rpc_channel_info_internal *mg_rpc_add_channel_internal(
    struct mg_rpc *c, const struct mg_str dst, struct mg_rpc_channel *ch);

static bool canonicalize_dst_uri(const struct mg_str sch,
                                 const struct mg_str user_info,
                                 const struct mg_str host, unsigned int port,
                                 const struct mg_str path,
                                 const struct mg_str qs, struct mg_str *uri) {
  return (mg_assemble_uri(&sch, &user_info, &host, port, &path, &qs,
                          NULL /* fragment */, 1 /* normalize_path */,
                          uri) == 0);
}

static bool dst_is_equal(const struct mg_str d1, const struct mg_str d2) {
  unsigned int port1, port2;
  struct mg_str sch1, ui1, host1, path1, qs1, f1;
  struct mg_str sch2, ui2, host2, path2, qs2, f2;
  bool iu1, iu2, result = false;
  iu1 = (mg_parse_uri(d1, &sch1, &ui1, &host1, &port1, &path1, &qs1, &f1) == 0);
  iu2 = (mg_parse_uri(d2, &sch2, &ui2, &host2, &port2, &path2, &qs2, &f2) == 0);
  if (!iu1 && !iu2) {
    result = (mg_strcmp(d1, d2) == 0);
  } else if (iu1 && iu2) {
    struct mg_str u1, u2;
    if (canonicalize_dst_uri(sch1, ui1, host1, port1, path1, qs1, &u1) &&
        canonicalize_dst_uri(sch2, ui2, host2, port2, path2, qs2, &u2)) {
      result = (mg_strcmp(u1, u2) == 0);
    }
    free((void *) u1.p);
    free((void *) u2.p);
  } else {
    /* URI vs simple ID comparisons remain undefined for now. */
    result = false;
  }
  return result;
}

static struct mg_rpc_channel_info_internal *
mg_rpc_get_channel_info_internal_by_dst(struct mg_rpc *c, struct mg_str *dst) {
  struct mg_rpc_channel_info_internal *ci;
  struct mg_rpc_channel_info_internal *default_ch = NULL;
  if (c == NULL) return NULL;
  struct mg_str scheme, user_info, host, path, query, fragment;
  unsigned int port = 0;
  bool is_uri =
      (dst->len > 0 && (mg_parse_uri(*dst, &scheme, &user_info, &host, &port,
                                     &path, &query, &fragment) == 0) &&
       scheme.len > 0);
  SLIST_FOREACH(ci, &c->channels, channels) {
    /* For implied destinations we use default route. */
    if (dst->len != 0 && dst_is_equal(*dst, ci->dst)) {
      goto out;
    }
    if (mg_vcmp(&ci->dst, MG_RPC_DST_DEFAULT) == 0) default_ch = ci;
  }
  /* If destination is a URI, maybe it tells us to open an outgoing channel. */
  if (is_uri) {
    /* At the moment we treat HTTP channels like WS */
    if (mg_vcmp(&scheme, "ws") == 0 || mg_vcmp(&scheme, "wss") == 0 ||
        mg_vcmp(&scheme, "http") == 0 || mg_vcmp(&scheme, "https") == 0) {
      char val_buf[MG_MAX_PATH];
      struct mg_rpc_channel_ws_out_cfg chcfg;
      memset(&chcfg, 0, sizeof(chcfg));
      struct mg_str canon_dst = MG_NULL_STR;
      canonicalize_dst_uri(scheme, user_info, host, port, path, query,
                           &canon_dst);
      chcfg.server_address = canon_dst;
#if MG_ENABLE_SSL
      if (mg_get_http_var(&fragment, "ssl_ca_file", val_buf, sizeof(val_buf)) >
          0) {
        chcfg.ssl_ca_file = mg_strdup(mg_mk_str(val_buf));
      }
      if (mg_get_http_var(&fragment, "ssl_client_cert_file", val_buf,
                          sizeof(val_buf)) > 0) {
        chcfg.ssl_client_cert_file = mg_strdup(mg_mk_str(val_buf));
      }
      if (mg_get_http_var(&fragment, "ssl_server_name", val_buf,
                          sizeof(val_buf)) > 0) {
        chcfg.ssl_server_name = mg_strdup(mg_mk_str(val_buf));
      }
#endif
      if (mg_get_http_var(&fragment, "reconnect_interval_min", val_buf,
                          sizeof(val_buf)) > 0) {
        chcfg.reconnect_interval_min = atoi(val_buf);
      } else {
        chcfg.reconnect_interval_min =
            mgos_sys_config_get_rpc_ws_reconnect_interval_min();
      }
      if (mg_get_http_var(&fragment, "reconnect_interval_max", val_buf,
                          sizeof(val_buf)) > 0) {
        chcfg.reconnect_interval_max = atoi(val_buf);
      } else {
        chcfg.reconnect_interval_max =
            mgos_sys_config_get_rpc_ws_reconnect_interval_max();
      }
      if (mg_get_http_var(&fragment, "idle_close_timeout", val_buf,
                          sizeof(val_buf)) > 0) {
        chcfg.idle_close_timeout = atoi(val_buf);
      } else {
        chcfg.idle_close_timeout =
            c->cfg->default_out_channel_idle_close_timeout;
      }

      struct mg_rpc_channel *ch = mg_rpc_channel_ws_out(mgos_get_mgr(), &chcfg);
      if (ch != NULL) {
        ci = mg_rpc_add_channel_internal(c, canon_dst, ch);
        if (ci != NULL) {
          ch->ch_connect(ch);
        }
      } else {
        LOG(LL_ERROR,
            ("Failed to create RPC channel from %.*s", (int) dst->len, dst->p));
        ci = NULL;
      }
      free((void *) canon_dst.p);
#if MG_ENABLE_SSL
      free((void *) chcfg.ssl_ca_file.p);
      free((void *) chcfg.ssl_client_cert_file.p);
      free((void *) chcfg.ssl_server_name.p);
#endif
    } else {
      LOG(LL_ERROR,
          ("Unsupported connection scheme in %.*s", (int) dst->len, dst->p));
      ci = NULL;
    }
  } else {
    ci = default_ch;
  }
out:
  LOG(LL_DEBUG, ("'%.*s' -> %p", (int) dst->len, dst->p, (ci ? ci->ch : NULL)));
  if (is_uri) {
    /*
     * For now, URI-based destinations are only implied, i.e. connections
     * are point to point.
     */
    dst->len = 0;
  }
  return ci;
}

static bool mg_rpc_handle_request(struct mg_rpc *c,
                                  struct mg_rpc_channel_info_internal *ci,
                                  const struct mg_rpc_frame *frame) {
  struct mg_rpc_request_info *ri =
      (struct mg_rpc_request_info *) calloc(1, sizeof(*ri));
  ri->rpc = c;
  ri->id = frame->id;
  ri->src = mg_strdup(frame->src);
  ri->dst = mg_strdup(frame->dst);
  ri->tag = mg_strdup(frame->tag);
  ri->auth = mg_strdup(frame->auth);
  ri->method = mg_strdup(frame->method);
  ri->ch = ci->ch;

  struct mg_rpc_handler_info *hi;
  SLIST_FOREACH(hi, &c->handlers, handlers) {
    if (mg_vcmp(&ri->method, hi->method) == 0) break;
  }
  if (hi == NULL) {
    LOG(LL_ERROR,
        ("No handler for %.*s", (int) frame->method.len, frame->method.p));
    mg_rpc_send_errorf(ri, 404, "No handler for %.*s", (int) frame->method.len,
                       frame->method.p);
    ri = NULL;
    return true;
  }
  struct mg_rpc_frame_info fi;
  memset(&fi, 0, sizeof(fi));
  fi.channel_type = ci->ch->get_type(ci->ch);
  ri->args_fmt = hi->args_fmt;

  bool ok = true;

  if (c->prehandler != NULL) {
    ok = c->prehandler(ri, c->prehandler_arg, &fi, frame->args);
  }

  if (ok) {
    hi->cb(ri, hi->cb_arg, &fi, frame->args);
  }

  return true;
}

static bool mg_rpc_handle_response(struct mg_rpc *c,
                                   struct mg_rpc_channel_info_internal *ci,
                                   int64_t id, struct mg_str result,
                                   int error_code, struct mg_str error_msg) {
  if (id == 0) {
    LOG(LL_ERROR, ("Response without an ID"));
    return false;
  }

  struct mg_rpc_sent_request_info *ri;
  SLIST_FOREACH(ri, &c->requests, requests) {
    if (ri->id == id) break;
  }
  if (ri == NULL) {
    /*
     * Response to a request we did not send.
     * Or (more likely) we did not request a response at all, so be quiet.
     */
    return true;
  }
  SLIST_REMOVE(&c->requests, ri, mg_rpc_sent_request_info, requests);
  struct mg_rpc_frame_info fi;
  memset(&fi, 0, sizeof(fi));
  fi.channel_type = ci->ch->get_type(ci->ch);
  ri->cb(c, ri->cb_arg, &fi, mg_mk_str_n(result.p, result.len), error_code,
         mg_mk_str_n(error_msg.p, error_msg.len));
  free(ri);
  return true;
}

bool mg_rpc_parse_frame(const struct mg_str f, struct mg_rpc_frame *frame) {
  memset(frame, 0, sizeof(*frame));

  struct json_token src, dst, tag;
  struct json_token method, args;
  struct json_token result, error_msg;
  struct json_token auth;
  memset(&src, 0, sizeof(src));
  memset(&dst, 0, sizeof(dst));
  memset(&tag, 0, sizeof(tag));
  memset(&method, 0, sizeof(method));
  memset(&args, 0, sizeof(args));
  memset(&result, 0, sizeof(result));
  memset(&error_msg, 0, sizeof(error_msg));
  memset(&auth, 0, sizeof(auth));

  if (json_scanf(f.p, f.len,
                 "{v:%d id:%lld src:%T dst:%T tag:%T"
                 "method:%T args:%T "
                 "auth:%T "
                 "result:%T error:{code:%d message:%T}}",
                 &frame->version, &frame->id, &src, &dst, &tag, &method, &args,
                 &auth, &result, &frame->error_code, &error_msg) < 1) {
    return false;
  }

  /*
   * Frozen returns string values without quotes, but we want quotes here, so
   * if the result is a string, "widen" it so that quotes are included.
   */
  if (result.type == JSON_TYPE_STRING) {
    result.ptr--;
    result.len += 2;
  }

  frame->src = mg_mk_str_n(src.ptr, src.len);
  frame->dst = mg_mk_str_n(dst.ptr, dst.len);
  frame->tag = mg_mk_str_n(tag.ptr, tag.len);
  frame->method = mg_mk_str_n(method.ptr, method.len);
  frame->args = mg_mk_str_n(args.ptr, args.len);
  frame->result = mg_mk_str_n(result.ptr, result.len);
  frame->error_msg = mg_mk_str_n(error_msg.ptr, error_msg.len);
  frame->auth = mg_mk_str_n(auth.ptr, auth.len);

  LOG(LL_DEBUG, ("%lld '%.*s' '%.*s' '%.*s'", (long long int) frame->id,
                 (int) src.len, (src.len > 0 ? src.ptr : ""), (int) dst.len,
                 (dst.len > 0 ? dst.ptr : ""), (int) method.len,
                 (method.len > 0 ? method.ptr : "")));

  return true;
}

static bool is_local_id(struct mg_rpc *c, const struct mg_str id) {
  for (size_t i = 0; i < c->local_ids.len;) {
    const struct mg_str local_id = mg_mk_str(c->local_ids.buf + i);
    if (mg_strcmp(id, local_id) == 0) return true;
    i += local_id.len + 1 /* NUL */;
  }
  return false;
}

static bool mg_rpc_handle_frame(struct mg_rpc *c,
                                struct mg_rpc_channel_info_internal *ci,
                                const struct mg_rpc_frame *frame) {
  if (!ci->is_open) {
    LOG(LL_ERROR, ("%p Ignored frame from closed channel (%s)", ci->ch,
                   ci->ch->get_type(ci->ch)));
    return false;
  }
  if (frame->dst.len != 0) {
    if (!is_local_id(c, frame->dst)) {
      LOG(LL_ERROR, ("Wrong dst: '%.*s'", (int) frame->dst.len, frame->dst.p));
      return false;
    }
  } else {
    /*
     * Implied destination is "whoever is on the other end", meaning us.
     */
  }
  /* If this channel did not have an associated address, record it now. */
  if (ci->dst.len == 0) {
    ci->dst = mg_strdup(frame->src);
  }
  if (frame->method.len > 0) {
    if (!mg_rpc_handle_request(c, ci, frame)) {
      return false;
    }
  } else {
    if (!mg_rpc_handle_response(c, ci, frame->id, frame->result,
                                frame->error_code, frame->error_msg)) {
      return false;
    }
  }
  return true;
}

static bool mg_rpc_send_frame(struct mg_rpc_channel_info_internal *ci,
                              struct mg_str frame);
static bool mg_rpc_dispatch_frame(
    struct mg_rpc *c, const struct mg_str src, const struct mg_str dst,
    int64_t id, const struct mg_str tag, const struct mg_str key,
    struct mg_rpc_channel_info_internal *ci, bool enqueue,
    struct mg_str payload_prefix_json, const char *payload_jsonf, va_list ap);

static void mg_rpc_remove_queue_entry(struct mg_rpc *c,
                                      struct mg_rpc_queue_entry *qe) {
  STAILQ_REMOVE(&c->queue, qe, mg_rpc_queue_entry, queue);
  free((void *) qe->dst.p);
  free((void *) qe->frame.p);
  memset(qe, 0, sizeof(*qe));
  free(qe);
  c->queue_len--;
}

static void mg_rpc_process_queue(struct mg_rpc *c) {
  struct mg_rpc_queue_entry *qe, *tqe;
  STAILQ_FOREACH_SAFE(qe, &c->queue, queue, tqe) {
    struct mg_rpc_channel_info_internal *ci = qe->ci;
    struct mg_str dst = qe->dst;
    if (ci == NULL) ci = mg_rpc_get_channel_info_internal_by_dst(c, &dst);
    if (mg_rpc_send_frame(ci, qe->frame)) {
      mg_rpc_remove_queue_entry(c, qe);
    }
  }
}

static void mg_rpc_ev_handler(struct mg_rpc_channel *ch,
                              enum mg_rpc_channel_event ev, void *ev_data) {
  struct mg_rpc *c = (struct mg_rpc *) ch->mg_rpc_data;
  struct mg_rpc_channel_info_internal *ci = NULL;
  SLIST_FOREACH(ci, &c->channels, channels) {
    if (ci->ch == ch) break;
  }
  /* This shouldn't happen, there must be info for all chans, but... */
  if (ci == NULL) return;
  switch (ev) {
    case MG_RPC_CHANNEL_OPEN: {
      ci->is_open = true;
      ci->is_busy = false;
      char *info = ch->get_info(ch);
      LOG(LL_DEBUG, ("%p CHAN OPEN (%s%s%s)", ch, ch->get_type(ch),
                     (info ? " " : ""), (info ? info : "")));
      free(info);
      mg_rpc_process_queue(c);
      if (ci->dst.len > 0) {
        mg_rpc_call_observers(c, MG_RPC_EV_CHANNEL_OPEN, &ci->dst);
      }
      break;
    }
    case MG_RPC_CHANNEL_FRAME_RECD: {
      const struct mg_str *f = (const struct mg_str *) ev_data;
      struct mg_rpc_frame frame;
      LOG(LL_DEBUG,
          ("%p GOT FRAME (%d): %.*s", ch, (int) f->len, (int) f->len, f->p));
      if (!mg_rpc_parse_frame(*f, &frame) ||
          !mg_rpc_handle_frame(c, ci, &frame)) {
        LOG(LL_ERROR, ("%p INVALID FRAME (%d): '%.*s'", ch, (int) f->len,
                       (int) f->len, f->p));
        if (!ch->is_persistent(ch)) ch->ch_close(ch);
      }
      break;
    }
    case MG_RPC_CHANNEL_FRAME_RECD_PARSED: {
      const struct mg_rpc_frame *frame = (const struct mg_rpc_frame *) ev_data;
      LOG(LL_DEBUG, ("%p GOT PARSED FRAME: '%.*s' -> '%.*s' %lld", ch,
                     (int) frame->src.len, (frame->src.p ? frame->src.p : ""),
                     (int) frame->dst.len, (frame->dst.p ? frame->dst.p : ""),
                     frame->id));
      if (!mg_rpc_handle_frame(c, ci, frame)) {
        LOG(LL_ERROR,
            ("%p INVALID PARSED FRAME from %.*s: %.*s %.*s", ch,
             (int) frame->src.len, frame->src.p, (int) frame->method.len,
             frame->method.p, (int) frame->args.len, frame->args.p));
        if (!ch->is_persistent(ch)) ch->ch_close(ch);
      }
      break;
    }
    case MG_RPC_CHANNEL_FRAME_SENT: {
      int success = (intptr_t) ev_data;
      LOG(LL_DEBUG, ("%p FRAME SENT (%d)", ch, success));
      ci->is_busy = false;
      mg_rpc_process_queue(c);
      (void) success;
      break;
    }
    case MG_RPC_CHANNEL_CLOSED: {
      bool remove = !ch->is_persistent(ch);
      LOG(LL_DEBUG, ("%p CHAN CLOSED, remove? %d", ch, remove));
      ci->is_open = ci->is_busy = false;
      if (ci->dst.len > 0) {
        mg_rpc_call_observers(c, MG_RPC_EV_CHANNEL_CLOSED, &ci->dst);
      }
      if (remove) {
        struct mg_rpc_queue_entry *qe, *tqe;
        STAILQ_FOREACH_SAFE(qe, &c->queue, queue, tqe) {
          if (qe->ci == ci) mg_rpc_remove_queue_entry(c, qe);
        }
        SLIST_REMOVE(&c->channels, ci, mg_rpc_channel_info_internal, channels);
        ch->ch_destroy(ch);
        if (ci->dst.p != NULL) free((void *) ci->dst.p);
        memset(ci, 0, sizeof(*ci));
        free(ci);
      }
      break;
    }
  }
}

static struct mg_rpc_channel_info_internal *mg_rpc_add_channel_internal(
    struct mg_rpc *c, const struct mg_str dst, struct mg_rpc_channel *ch) {
  struct mg_rpc_channel_info_internal *ci =
      (struct mg_rpc_channel_info_internal *) calloc(1, sizeof(*ci));
  if (dst.len != 0) ci->dst = mg_strdup(dst);
  ci->ch = ch;
  ch->mg_rpc_data = c;
  ch->ev_handler = mg_rpc_ev_handler;
  SLIST_INSERT_HEAD(&c->channels, ci, channels);
  LOG(LL_DEBUG, ("%p '%.*s' %s", ch, (int) dst.len, dst.p, ch->get_type(ch)));
  return ci;
}

void mg_rpc_add_channel(struct mg_rpc *c, const struct mg_str dst,
                        struct mg_rpc_channel *ch) {
  mg_rpc_add_channel_internal(c, dst, ch);
}

void mg_rpc_connect(struct mg_rpc *c) {
  struct mg_rpc_channel_info_internal *ci;
  SLIST_FOREACH(ci, &c->channels, channels) {
    ci->ch->ch_connect(ci->ch);
  }
}

void mg_rpc_disconnect(struct mg_rpc *c) {
  struct mg_rpc_channel_info_internal *ci;
  SLIST_FOREACH(ci, &c->channels, channels) {
    ci->ch->ch_close(ci->ch);
  }
}

void mg_rpc_add_local_id(struct mg_rpc *c, const struct mg_str id) {
  if (id.len == 0) return;
  mbuf_append(&c->local_ids, id.p, id.len);
  mbuf_append(&c->local_ids, "", 1); /* Add NUL */
}

struct mg_rpc *mg_rpc_create(struct mg_rpc_cfg *cfg) {
  struct mg_rpc *c = (struct mg_rpc *) calloc(1, sizeof(*c));
  if (c == NULL) return NULL;
  c->cfg = cfg;
  mbuf_init(&c->local_ids, 0);
  mg_rpc_add_local_id(c, mg_mk_str(c->cfg->id));

  SLIST_INIT(&c->handlers);
  SLIST_INIT(&c->channels);
  SLIST_INIT(&c->requests);
  SLIST_INIT(&c->observers);
  STAILQ_INIT(&c->queue);

  return c;
}

static bool mg_rpc_send_frame(struct mg_rpc_channel_info_internal *ci,
                              const struct mg_str f) {
  if (ci == NULL || !ci->is_open || ci->is_busy) return false;
  bool result = ci->ch->send_frame(ci->ch, f);
  LOG(LL_DEBUG, ("%p SEND FRAME (%d): %.*s -> %d", ci->ch, (int) f.len,
                 (int) f.len, f.p, result));
  if (result) ci->is_busy = true;
  return result;
}

static bool mg_rpc_enqueue_frame(struct mg_rpc *c,
                                 struct mg_rpc_channel_info_internal *ci,
                                 struct mg_str dst, struct mg_str f) {
  if (c->queue_len >= c->cfg->max_queue_length) return false;
  struct mg_rpc_queue_entry *qe =
      (struct mg_rpc_queue_entry *) calloc(1, sizeof(*qe));
  qe->dst = mg_strdup(dst);
  qe->ci = ci;
  qe->frame = f;
  STAILQ_INSERT_TAIL(&c->queue, qe, queue);
  LOG(LL_DEBUG, ("QUEUED FRAME (%d): %.*s", (int) f.len, (int) f.len, f.p));
  c->queue_len++;
  return true;
}

static bool mg_rpc_dispatch_frame(
    struct mg_rpc *c, const struct mg_str src, const struct mg_str dst,
    int64_t id, const struct mg_str tag, const struct mg_str key,
    struct mg_rpc_channel_info_internal *ci, bool enqueue,
    struct mg_str payload_prefix_json, const char *payload_jsonf, va_list ap) {
  struct mbuf fb;
  struct json_out fout = JSON_OUT_MBUF(&fb);
  struct mg_str final_dst = dst;
  if (ci == NULL) ci = mg_rpc_get_channel_info_internal_by_dst(c, &final_dst);
  bool result = false;
  mbuf_init(&fb, 100);
  json_printf(&fout, "{");
  if (id != 0) {
    json_printf(&fout, "id:%lld,", id);
  }
  if (src.len > 0) {
    json_printf(&fout, "src:%.*Q", (int) src.len, src.p);
  } else {
    json_printf(&fout, "src:%Q", c->local_ids.buf);
  }
  if (final_dst.len > 0) {
    json_printf(&fout, ",dst:%.*Q", (int) final_dst.len, final_dst.p);
  }
  if (tag.len > 0) {
    json_printf(&fout, ",tag:%.*Q", (int) tag.len, tag.p);
  }
  if (key.len > 0) {
    json_printf(&fout, ",key:%.*Q", (int) key.len, key.p);
  }
  if (payload_prefix_json.len > 0) {
    mbuf_append(&fb, ",", 1);
    mbuf_append(&fb, payload_prefix_json.p, payload_prefix_json.len);
  }
  if (payload_jsonf != NULL) json_vprintf(&fout, payload_jsonf, ap);
  json_printf(&fout, "}");
  mbuf_trim(&fb);

  /* Try sending directly first or put on the queue. */
  struct mg_str f = mg_mk_str_n(fb.buf, fb.len);
  if (mg_rpc_send_frame(ci, f)) {
    mbuf_free(&fb);
    result = true;
  } else if (enqueue && mg_rpc_enqueue_frame(c, ci, dst, f)) {
    /* Frame is on the queue, do not free. */
    result = true;
  } else {
    LOG(LL_DEBUG,
        ("DROPPED FRAME (%d): %.*s", (int) fb.len, (int) fb.len, fb.buf));
    mbuf_free(&fb);
  }
  return result;
}

bool mg_rpc_vcallf(struct mg_rpc *c, const struct mg_str method,
                   mg_result_cb_t cb, void *cb_arg,
                   const struct mg_rpc_call_opts *opts, const char *args_jsonf,
                   va_list ap) {
  if (c == NULL) return false;
  struct mbuf prefb;
  struct json_out prefbout = JSON_OUT_MBUF(&prefb);
  int64_t id = mg_rpc_get_id(c);
  struct mg_str dst = MG_NULL_STR, tag = MG_NULL_STR, key = MG_NULL_STR;
  if (opts != NULL && opts->dst.len > 0) dst = opts->dst;
  if (opts != NULL && opts->tag.len > 0) tag = opts->tag;
  if (opts != NULL && opts->key.len > 0) key = opts->key;
  struct mg_rpc_sent_request_info *ri = NULL;
  mbuf_init(&prefb, 100);
  if (cb != NULL) {
    ri = (struct mg_rpc_sent_request_info *) calloc(1, sizeof(*ri));
    ri->id = id;
    ri->cb = cb;
    ri->cb_arg = cb_arg;
  } else {
    /* No callback - put marker in the frame that no response is expected */
    json_printf(&prefbout, "nr:%B,", true);
  }
  json_printf(&prefbout, "method:%.*Q", (int) method.len, method.p);
  if (args_jsonf != NULL) json_printf(&prefbout, ",args:");
  const struct mg_str pprefix = mg_mk_str_n(prefb.buf, prefb.len);
  struct mg_str src = opts->src;
  if (src.len == 0) src = mg_mk_str(c->cfg->id);

  bool result = false;
  if (!opts->broadcast) {
    bool enqueue = (opts == NULL ? true : !opts->no_queue);
    result = mg_rpc_dispatch_frame(c, src, dst, id, tag, key, NULL /* ci */,
                                   enqueue, pprefix, args_jsonf, ap);
  } else {
    struct mg_rpc_channel_info_internal *ci;
    SLIST_FOREACH(ci, &c->channels, channels) {
      if (ci->ch->is_broadcast_enabled == NULL ||
          !ci->ch->is_broadcast_enabled(ci->ch)) {
        continue;
      }
      result |=
          mg_rpc_dispatch_frame(c, src, dst, id, tag, key, ci,
                                false /* enqueue */, pprefix, args_jsonf, ap);
    }
  }
  mbuf_free(&prefb);

  if (result && ri != NULL) {
    SLIST_INSERT_HEAD(&c->requests, ri, requests);
    return true;
  } else {
    /* Could not send or queue, drop on the floor. */
    free(ri);
    return false;
  }
}

bool mg_rpc_callf(struct mg_rpc *c, const struct mg_str method,
                  mg_result_cb_t cb, void *cb_arg,
                  const struct mg_rpc_call_opts *opts, const char *args_jsonf,
                  ...) {
  va_list ap;
  va_start(ap, args_jsonf);
  bool res = mg_rpc_vcallf(c, method, cb, cb_arg, opts, args_jsonf, ap);
  va_end(ap);
  return res;
}

bool mg_rpc_send_responsef(struct mg_rpc_request_info *ri,
                           const char *result_json_fmt, ...) {
  struct mbuf prefb;
  bool result = true;
  va_list ap;
  struct mg_str key = MG_NULL_STR;
  struct mg_rpc_channel_info_internal *ci;
  if (result_json_fmt == NULL) return mg_rpc_send_responsef(ri, "%s", "null");
  ci = mg_rpc_get_channel_info_internal(ri->rpc, ri->ch);
  mbuf_init(&prefb, 15);
  mbuf_append(&prefb, "\"result\":", 9);
  va_start(ap, result_json_fmt);
  result = mg_rpc_dispatch_frame(
      ri->rpc, ri->dst, ri->src, ri->id, ri->tag, key, ci, true /* enqueue */,
      mg_mk_str_n(prefb.buf, prefb.len), result_json_fmt, ap);
  va_end(ap);
  mg_rpc_free_request_info(ri);
  mbuf_free(&prefb);
  return result;
}

static bool send_errorf(struct mg_rpc_request_info *ri, int error_code,
                        int is_json, const char *error_msg_fmt, va_list ap) {
  struct mbuf prefb;
  struct json_out prefbout = JSON_OUT_MBUF(&prefb);
  mbuf_init(&prefb, 0);
  json_printf(&prefbout, "error:{code:%d", error_code);
  if (error_msg_fmt != NULL) {
    if (is_json) {
      struct mbuf msgb;
      struct json_out msgbout = JSON_OUT_MBUF(&msgb);
      mbuf_init(&msgb, 0);

      if (json_vprintf(&msgbout, error_msg_fmt, ap) > 0) {
        json_printf(&prefbout, ",message:%.*Q", msgb.len, msgb.buf);
      }

      mbuf_free(&msgb);
    } else {
      char buf[100], *msg = buf;
      if (mg_avprintf(&msg, sizeof(buf), error_msg_fmt, ap) > 0) {
        json_printf(&prefbout, ",message:%Q", msg);
      }
      if (msg != buf) free(msg);
    }
  }
  json_printf(&prefbout, "}");
  va_list dummy;
  memset(&dummy, 0, sizeof(dummy));
  struct mg_rpc_channel_info_internal *ci =
      mg_rpc_get_channel_info_internal(ri->rpc, ri->ch);
  struct mg_str key = MG_NULL_STR;
  bool result = mg_rpc_dispatch_frame(
      ri->rpc, ri->dst, ri->src, ri->id, ri->tag, key, ci, true /* enqueue */,
      mg_mk_str_n(prefb.buf, prefb.len), NULL, dummy);
  mg_rpc_free_request_info(ri);
  mbuf_free(&prefb);
  return result;
}

bool mg_rpc_send_errorf(struct mg_rpc_request_info *ri, int error_code,
                        const char *error_msg_fmt, ...) {
  va_list ap;
  va_start(ap, error_msg_fmt);
  bool ret = send_errorf(ri, error_code, 0 /* not json */, error_msg_fmt, ap);
  va_end(ap);
  return ret;
}

bool mg_rpc_send_error_jsonf(struct mg_rpc_request_info *ri, int error_code,
                             const char *error_msg_fmt, ...) {
  va_list ap;
  va_start(ap, error_msg_fmt);
  bool ret = send_errorf(ri, error_code, 1 /* json */, error_msg_fmt, ap);
  va_end(ap);
  return ret;
}

bool mg_rpc_check_digest_auth(struct mg_rpc_request_info *ri) {
  if (ri->authn_info.username.len > 0) {
    LOG(LL_DEBUG,
        ("Already have username in request info: \"%.*s\", skip checking",
         (int) ri->authn_info.username.len, ri->authn_info.username.p));
    return true;
  }

  if (ri->auth.len > 0) {
    struct json_token trealm = JSON_INVALID_TOKEN,
                      tusername = JSON_INVALID_TOKEN,
                      tnonce = JSON_INVALID_TOKEN, tcnonce = JSON_INVALID_TOKEN,
                      tresponse = JSON_INVALID_TOKEN;

    if (json_scanf(ri->auth.p, ri->auth.len,
                   "{realm: %T username %T nonce:%T cnonce:%T response:%T}",
                   &trealm, &tusername, &tnonce, &tcnonce, &tresponse) == 5) {
      struct mg_str realm = mg_mk_str_n(trealm.ptr, trealm.len);
      struct mg_str username = mg_mk_str_n(tusername.ptr, tusername.len);
      struct mg_str nonce = mg_mk_str_n(tnonce.ptr, tnonce.len);
      struct mg_str cnonce = mg_mk_str_n(tcnonce.ptr, tcnonce.len);
      struct mg_str response = mg_mk_str_n(tresponse.ptr, tresponse.len);

      LOG(LL_DEBUG, ("Got auth: Realm:%.*s, Username:%.*s, Nonce: %.*s, "
                     "CNonce:%.*s, Response:%.*s",
                     (int) realm.len, realm.p, (int) username.len, username.p,
                     (int) nonce.len, nonce.p, (int) cnonce.len, cnonce.p,
                     (int) response.len, response.p));

      if (mg_vcmp(&realm, mgos_sys_config_get_rpc_auth_domain()) != 0) {
        LOG(LL_WARN,
            ("Got auth request with different realm: expected: "
             "\"%s\", got: \"%.*s\"",
             mgos_sys_config_get_rpc_auth_domain(), realm.len, realm.p));
      } else {
        FILE *htdigest_fp = fopen(mgos_sys_config_get_rpc_auth_file(), "r");

        if (htdigest_fp == NULL) {
          mg_rpc_send_errorf(ri, 500, "failed to open htdigest file");
          ri = NULL;
          return false;
        }

        /*
         * TODO(dfrank): add method to the struct mg_rpc_request_info and use
         * it as either method or uri
         */
        int authenticated = mg_check_digest_auth(
            mg_mk_str("dummy_method"), mg_mk_str("dummy_uri"), username, cnonce,
            response, mg_mk_str("auth"), mg_mk_str("1"), nonce, realm,
            htdigest_fp);

        fclose(htdigest_fp);

        LOG(LL_DEBUG, ("Authenticated:%d", authenticated));

        if (authenticated) {
          ri->authn_info.username = mg_strdup(username);
          return true;
        }
      }
    } else {
      LOG(LL_WARN, ("Not all auth parts are present, ignoring"));
    }
  }

  /*
   * Authentication has failed. NOTE: we're returning true to indicate that ri
   * is still valid and the caller can proceed to other authn means, if any.
   */

  return true;
}

void mg_rpc_add_handler(struct mg_rpc *c, const char *method,
                        const char *args_fmt, mg_handler_cb_t cb,
                        void *cb_arg) {
  if (c == NULL) return;
  struct mg_rpc_handler_info *hi =
      (struct mg_rpc_handler_info *) calloc(1, sizeof(*hi));
  hi->method = method;
  hi->cb = cb;
  hi->cb_arg = cb_arg;
  hi->args_fmt = args_fmt;
  SLIST_INSERT_HEAD(&c->handlers, hi, handlers);
}

void mg_rpc_set_prehandler(struct mg_rpc *c, mg_prehandler_cb_t cb,
                           void *cb_arg) {
  c->prehandler = cb;
  c->prehandler_arg = cb_arg;
}

bool mg_rpc_is_connected(struct mg_rpc *c) {
  struct mg_str dd = mg_mk_str(MG_RPC_DST_DEFAULT);
  struct mg_rpc_channel_info_internal *ci =
      mg_rpc_get_channel_info_internal_by_dst(c, &dd);
  return (ci != NULL && ci->is_open);
}

bool mg_rpc_can_send(struct mg_rpc *c) {
  struct mg_str dd = mg_mk_str(MG_RPC_DST_DEFAULT);
  struct mg_rpc_channel_info_internal *ci =
      mg_rpc_get_channel_info_internal_by_dst(c, &dd);
  return (ci != NULL && ci->is_open && !ci->is_busy);
}

void mg_rpc_free_request_info(struct mg_rpc_request_info *ri) {
  free((void *) ri->src.p);
  free((void *) ri->dst.p);
  free((void *) ri->tag.p);
  free((void *) ri->method.p);
  free((void *) ri->auth.p);
  mg_rpc_authn_info_free(&ri->authn_info);
  memset(ri, 0, sizeof(*ri));
  free(ri);
}

void mg_rpc_add_observer(struct mg_rpc *c, mg_observer_cb_t cb, void *cb_arg) {
  if (c == NULL) return;
  struct mg_rpc_observer_info *oi =
      (struct mg_rpc_observer_info *) calloc(1, sizeof(*oi));
  oi->cb = cb;
  oi->cb_arg = cb_arg;
  SLIST_INSERT_HEAD(&c->observers, oi, observers);
}

void mg_rpc_remove_observer(struct mg_rpc *c, mg_observer_cb_t cb,
                            void *cb_arg) {
  if (c == NULL) return;
  struct mg_rpc_observer_info *oi, *oit;
  SLIST_FOREACH_SAFE(oi, &c->observers, observers, oit) {
    if (oi->cb == cb && oi->cb_arg == cb_arg) {
      SLIST_REMOVE(&c->observers, oi, mg_rpc_observer_info, observers);
      free(oi);
      break;
    }
  }
}

void mg_rpc_free(struct mg_rpc *c) {
  /* FIXME(rojer): free other stuff */
  mbuf_free(&c->local_ids);
  free(c);
}

/* Return list of all registered RPC endpoints */
static void mg_rpc_list_handler(struct mg_rpc_request_info *ri, void *cb_arg,
                                struct mg_rpc_frame_info *fi,
                                struct mg_str args) {
  struct mg_rpc_handler_info *hi;
  struct mbuf mbuf;
  struct json_out out = JSON_OUT_MBUF(&mbuf);

  mbuf_init(&mbuf, 200);
  json_printf(&out, "[");
  SLIST_FOREACH(hi, &ri->rpc->handlers, handlers) {
    if (mbuf.len > 1) json_printf(&out, ",");
    json_printf(&out, "%Q", hi->method);
  }
  json_printf(&out, "]");

  mg_rpc_send_responsef(ri, "%.*s", mbuf.len, mbuf.buf);
  mbuf_free(&mbuf);

  (void) cb_arg;
  (void) args;
  (void) fi;
}

/* Describe a registered RPC endpoint */
static void mg_rpc_describe_handler(struct mg_rpc_request_info *ri,
                                    void *cb_arg, struct mg_rpc_frame_info *fi,
                                    struct mg_str args) {
  struct mg_rpc_handler_info *hi;
  struct json_token t = JSON_INVALID_TOKEN;
  if (json_scanf(args.p, args.len, ri->args_fmt, &t) != 1) {
    mg_rpc_send_errorf(ri, 400, "name is required");
    return;
  }
  struct mg_str name = mg_mk_str_n(t.ptr, t.len);
  SLIST_FOREACH(hi, &ri->rpc->handlers, handlers) {
    if (mg_vcmp(&name, hi->method) == 0) {
      struct mbuf mbuf;
      struct json_out out = JSON_OUT_MBUF(&mbuf);
      mbuf_init(&mbuf, 100);
      json_printf(&out, "{name: %.*Q, args_fmt: %Q}", t.len, t.ptr,
                  hi->args_fmt);
      mg_rpc_send_responsef(ri, "%.*s", mbuf.len, mbuf.buf);
      mbuf_free(&mbuf);
      return;
    }
  }
  mg_rpc_send_errorf(ri, 404, "name not found");
  (void) cb_arg;
  (void) fi;
}

/* Reply with the peer info */
static void mg_rpc_ping_handler(struct mg_rpc_request_info *ri, void *cb_arg,
                                struct mg_rpc_frame_info *fi,
                                struct mg_str args) {
  char *info = ri->ch->get_info(ri->ch);
  mg_rpc_send_responsef(ri, "{channel_info: %Q}", info == NULL ? "" : info);
  free(info);
  (void) fi;
  (void) cb_arg;
  (void) args;
}

void mg_rpc_authn_info_free(struct mg_rpc_authn_info *authn) {
  free((void *) authn->username.p);
  authn->username = mg_mk_str(NULL);
}

bool mg_rpc_get_channel_info(struct mg_rpc *c, struct mg_rpc_channel_info **ci,
                             int *num_ci) {
  *num_ci = 0;
  *ci = NULL;
  if (c == NULL) return false;
  bool result = true;
  struct mg_rpc_channel_info_internal *cii;
  SLIST_FOREACH(cii, &c->channels, channels) {
    struct mg_rpc_channel *ch = cii->ch;
    struct mg_rpc_channel_info *new_ci = (struct mg_rpc_channel_info *) realloc(
        *ci, ((*num_ci) + 1) * sizeof(**ci));
    if (new_ci == NULL) {
      result = false;
      goto clean;
    }
    struct mg_rpc_channel_info *r = &new_ci[*num_ci];
    memset(r, 0, sizeof(*r));
    r->dst = mg_strdup(cii->dst);
    r->type = mg_strdup(mg_mk_str(ch->get_type(ch)));
    r->info = mg_mk_str(ch->get_info(ch));
    r->is_open = cii->is_open;
    r->is_persistent = ch->is_persistent(ch);
    r->is_broadcast_enabled = ch->is_broadcast_enabled(ch);
    *ci = new_ci;
    (*num_ci)++;
  }
clean:
  if (!result) {
    mg_rpc_channel_info_free_all(*ci, *num_ci);
    *ci = NULL;
    *num_ci = 0;
  }
  return result;
}

void mg_rpc_channel_info_free(struct mg_rpc_channel_info *ci) {
  free((void *) ci->dst.p);
  free((void *) ci->type.p);
  free((void *) ci->info.p);
}

void mg_rpc_channel_info_free_all(struct mg_rpc_channel_info *cici,
                                  int num_ci) {
  for (int i = 0; i < num_ci; i++) {
    mg_rpc_channel_info_free(&cici[i]);
  }
  free(cici);
}

void mg_rpc_add_list_handler(struct mg_rpc *c) {
  mg_rpc_add_handler(c, "RPC.List", "", mg_rpc_list_handler, NULL);
  mg_rpc_add_handler(c, "RPC.Describe", "{name: %T}", mg_rpc_describe_handler,
                     NULL);
  mg_rpc_add_handler(c, "RPC.Ping", "", mg_rpc_ping_handler, NULL);
}
