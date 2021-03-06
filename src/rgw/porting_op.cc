/*
 * =====================================================================================
 *       Copyright (c), 2013-2020, Sobey.
 *       Filename:  porting_op.cc
 *
 *    Description:  
 *         Others:
 *
 *        Version:  1.0
 *        Created:  2015/12/11 14:31:41
 *       Revision:  none
 *       Compiler:  gcc
 *
 *         Author:  Sean. Hou (hwt), houwentaoff@gmail.com
 *   Organization:  Sobey
 *
 * =====================================================================================
 */
#include <errno.h>
#include <stdlib.h>

#include <sstream>

#include "common/Clock.h"
#include "common/armor.h"
#include "common/mime.h"
#include "common/utf8.h"
#include "common/ceph_json.h"

#include "porting_op.h"
#include "porting_common.h"
#include "porting_user.h"
#include "porting_rest.h"
#include "porting_rados.h"
#include "porting_acl.h"
#include "porting_acl_s3.h"
#include "include/rados/librados.hh"
#include "common/shell.h"
#include "common/ceph_crypto.h"
#include "rgw_multi.h"

#include "cgw/cgw.h"
#include "global/global.h"

#define MULTIPART_UPLOAD_ID_PREFIX_LEGACY "2/"
#define MULTIPART_UPLOAD_ID_PREFIX "2~" // must contain a unique char that may not come up in gen_rand_alpha()

using namespace std;
using ceph::crypto::MD5;

static string mp_ns = RGW_OBJ_NS_MULTIPART;
static string shadow_ns = RGW_OBJ_NS_SHADOW;

static int parse_range(const char *range, off_t& ofs, off_t& end, bool *partial_content)
{
  int r = -ERANGE;
  string s(range);
  string ofs_str;
  string end_str;

  *partial_content = false;

  int pos = s.find("bytes=");
  if (pos < 0) {
    pos = 0;
    while (isspace(s[pos]))
      pos++;
    int end = pos;
    while (isalpha(s[end]))
      end++;
    if (strncasecmp(s.c_str(), "bytes", end - pos) != 0)
      return 0;
    while (isspace(s[end]))
      end++;
    if (s[end] != '=')
      return 0;
    s = s.substr(end + 1);
  } else {
    s = s.substr(pos + 6); /* size of("bytes=")  */
  }
  pos = s.find('-');
  if (pos < 0)
    goto done;

  *partial_content = true;

  ofs_str = s.substr(0, pos);
  end_str = s.substr(pos + 1);
  if (end_str.length()) {
    end = atoll(end_str.c_str());
    if (end < 0)
      goto done;
  }

  if (ofs_str.length()) {
    ofs = atoll(ofs_str.c_str());
  } else { // RFC2616 suffix-byte-range-spec
    ofs = -end;
    end = -1;
  }

  if (end >= 0 && end < ofs)
    goto done;

  r = 0;
done:
  return r;
}

static bool object_is_expired(map<string, bufferlist>& attrs) {
  map<string, bufferlist>::iterator iter = attrs.find(RGW_ATTR_DELETE_AT);
  if (iter != attrs.end()) {
    utime_t delete_at;
    try {
      ::decode(delete_at, iter->second);
    } catch (buffer::error& err) {
      dout(0) << "ERROR: " << __func__ << ": failed to decode " RGW_ATTR_DELETE_AT " attr" << dendl;
      return false;
    }

    if (delete_at <= ceph_clock_now(NULL)) {
      return true;
    }
  }

  return false;
}

int RGWOp::verify_op_mask()
{
  uint32_t required_mask = op_mask();

//  ldout(s->cct, 20) << "required_mask= " << required_mask << " user.op_mask=" << s->user.op_mask << dendl;

//  if ((s->user.op_mask & required_mask) != required_mask) {
//    return -EPERM;
//  }

  if (!s->system_request && (required_mask & RGW_OP_TYPE_MODIFY) && 0/*  && !store->zone.is_master */)  {
    ldout(s->cct, 5) << "NOTICE: modify request to a non-master zone by a non-system user, permission denied"  << dendl;
    return -EPERM;
  }

  return 0;
}
#if 0
int RGWOp::init_quota()
{
  /* no quota enforcement for system requests */
  if (s->system_request)
    return 0;

  /* init quota related stuff */
  if (!(s->user.op_mask & RGW_OP_TYPE_MODIFY)) {
    return 0;
  }

  /* only interested in object related ops */
  if (s->object.empty()) {
    return 0;
  }

  RGWUserInfo owner_info;
  RGWUserInfo *uinfo;

  if (s->user.user_id == s->bucket_owner.get_id()) {
    uinfo = &s->user;
  } else {
    int r = rgw_get_user_info_by_uid(store, s->bucket_info.owner, owner_info);
    if (r < 0)
      return r;
    uinfo = &owner_info;
  }

  if (s->bucket_info.quota.enabled) {
    bucket_quota = s->bucket_info.quota;
  } else if (uinfo->bucket_quota.enabled) {
    bucket_quota = uinfo->bucket_quota;
  } else {
    bucket_quota = store->region_map.bucket_quota;
  }

  if (uinfo->user_quota.enabled) {
    user_quota = uinfo->user_quota;
  } else {
    user_quota = store->region_map.user_quota;
  }

  return 0;
}
#endif
int RGWOp::read_bucket_cors()
{
#if 0
  bufferlist bl;

  map<string, bufferlist>::iterator aiter = s->bucket_attrs.find(RGW_ATTR_CORS);
  if (aiter == s->bucket_attrs.end()) {
    ldout(s->cct, 20) << "no CORS configuration attr found" << dendl;
    cors_exist = false;
    return 0; /* no CORS configuration found */
  }

  cors_exist = true;

  bl = aiter->second;

  bufferlist::iterator iter = bl.begin();
  try {
    bucket_cors.decode(iter);
  } catch (buffer::error& err) {
    ldout(s->cct, 0) << "ERROR: could not decode policy, caught buffer::error" << dendl;
    return -EIO;
  }
  if (s->cct->_conf->subsys.should_gather(ceph_subsys_rgw, 15)) {
    RGWCORSConfiguration_S3 *s3cors = static_cast<RGWCORSConfiguration_S3 *>(&bucket_cors);
    ldout(s->cct, 15) << "Read RGWCORSConfiguration";
    s3cors->to_xml(*_dout);
    *_dout << dendl;
  }
#endif
  return 0;
}
RGWHandler::~RGWHandler()
{
}

int RGWHandler::init(RGWRados *_store, struct req_state *_s, RGWClientIO *cio)
{
  store = _store;
  s = _s;

  return 0;
}
static int decode_policy(CephContext *cct, bufferlist& bl, RGWAccessControlPolicy *policy)
{
  bufferlist::iterator iter = bl.begin();
  try {
    policy->decode(iter);
  } catch (buffer::error& err) {
    ldout(cct, 0) << "ERROR: could not decode policy, caught buffer::error" << dendl;
    return -EIO;
  }
  if (1/*cct->_conf->subsys.should_gather(ceph_subsys_rgw, 15)*/) {
    RGWAccessControlPolicy_S3 *s3policy = static_cast<RGWAccessControlPolicy_S3 *>(policy);
    ldout(cct, 15) << "Read AccessControlPolicy";
    s3policy->to_xml(*_dout);
    *_dout << dendl;
  }
  return 0;
}

static int get_bucket_policy_from_attr(CephContext *cct, RGWRados *store, void *ctx,
                                       RGWBucketInfo& bucket_info, map<string, bufferlist>& bucket_attrs,
                                       RGWAccessControlPolicy *policy, rgw_obj& obj)
{
  map<string, bufferlist>::iterator aiter = bucket_attrs.find(RGW_ATTR_ACL);

  if (aiter != bucket_attrs.end()) {
    int ret = decode_policy(cct, aiter->second, policy);
    if (ret < 0)
      return ret;
  } else {
    ldout(cct, 0) << "WARNING: couldn't find acl header for bucket, generating default" << dendl;
    RGWUserInfo uinfo;
    /* object exists, but policy is broken */
    int r = rgw_get_user_info_by_uid(store, bucket_info.owner, uinfo);
    if (r < 0)
      return r;

    policy->create_default(bucket_info.owner, uinfo.display_name);
  }
  return 0;
}

static int get_obj_policy_from_attr(CephContext *cct, RGWRados *store, RGWObjectCtx& obj_ctx,
                                    RGWBucketInfo& bucket_info, map<string, bufferlist>& bucket_attrs,
                                    RGWAccessControlPolicy *policy, rgw_obj& obj)
{
  bufferlist bl;
  int ret = 0;

  RGWRados::Object op_target(store, bucket_info, obj_ctx, obj);
  RGWRados::Object::Read rop(&op_target);

  ret = rop.get_attr(RGW_ATTR_ACL, bl);
  if (ret >= 0) {
    ret = decode_policy(cct, bl, policy);
    if (ret < 0)
      return ret;
  } else if (ret == -ENODATA) {
    /* object exists, but policy is broken */
    ldout(cct, 0) << "WARNING: couldn't find acl header for object, generating default" << dendl;
    RGWUserInfo uinfo;
    ret = rgw_get_user_info_by_uid(store, bucket_info.owner, uinfo);
    if (ret < 0)
      return ret;

    policy->create_default(bucket_info.owner, uinfo.display_name);
  }
  return ret;
}

/**
 * Get the AccessControlPolicy for an object off of disk.
 * policy: must point to a valid RGWACL, and will be filled upon return.
 * bucket: name of the bucket containing the object.
 * object: name of the object to get the ACL for.
 * Returns: 0 on success, -ERR# otherwise.
 */
static int get_policy_from_attr(CephContext *cct, RGWRados *store, void *ctx,
                                RGWBucketInfo& bucket_info, map<string, bufferlist>& bucket_attrs,
                                RGWAccessControlPolicy *policy, rgw_obj& obj)
{
  if (obj.bucket.name.empty()) {
    return 0;
  }

  if (obj.get_object().empty()) {
    rgw_obj instance_obj;
    //store->get_bucket_instance_obj(bucket_info.bucket, instance_obj);
    return get_bucket_policy_from_attr(cct, store, ctx, bucket_info, bucket_attrs,
                                       policy, instance_obj);
  }
  return get_obj_policy_from_attr(cct, store, *static_cast<RGWObjectCtx *>(ctx), bucket_info, bucket_attrs,
                                  policy, obj);
}

static int read_policy(RGWRados *store, struct req_state *s,
                       RGWBucketInfo& bucket_info, map<string, bufferlist>& bucket_attrs,
                       RGWAccessControlPolicy *policy, rgw_bucket& bucket, rgw_obj_key& object)
{
  string upload_id;
  upload_id = s->info.args.get("uploadId");
  rgw_obj obj;

  if (!s->system_request && bucket_info.flags & BUCKET_SUSPENDED) {
    ldout(s->cct, 0) << "NOTICE: bucket " << bucket_info.bucket.name << " is suspended" << dendl;
    return -ERR_USER_SUSPENDED;
  }

  if (!object.empty() && !upload_id.empty()) {
    /* multipart upload */
    RGWMPObj mp(object.name, upload_id);
    string oid = mp.get_meta();
    obj.init_ns(bucket, oid, mp_ns);
    obj.set_in_extra_data(true);
  } else {
    obj = rgw_obj(bucket, object);
  }
  int ret = get_policy_from_attr(s->cct, store, s->obj_ctx, bucket_info, bucket_attrs, policy, obj);
  policy->owner.id = "bwcpn";//("<ID>bwcpn</ID>");// = "bwcpn";

  if (ret == -ENOENT && !object.empty()) {
    /* object does not exist checking the bucket's ACL to make sure
       that we send a proper error code */
    RGWAccessControlPolicy bucket_policy(s->cct);
    string no_object;
    rgw_obj no_obj(bucket, no_object);
    ret = get_policy_from_attr(s->cct, store, s->obj_ctx, bucket_info, bucket_attrs, &bucket_policy, no_obj);
    if (ret < 0)
      return ret;
    string& owner = bucket_policy.get_owner().get_id();
    if (!s->system_request && owner.compare(s->user.user_id) != 0 &&
        !bucket_policy.verify_permission(s->user.user_id, s->perm_mask, RGW_PERM_READ))
      ret = -EACCES;
    else
      ret = -ENOENT;

  } else if (ret == -ENOENT) {
      ret = -ERR_NO_SUCH_BUCKET;
  }

  return ret;
}


/**
 * Get the AccessControlPolicy for a bucket or object off of disk.
 * s: The req_state to draw information from.
 * only_bucket: If true, reads the bucket ACL rather than the object ACL.
 * Returns: 0 on success, -ERR# otherwise.
 */
static int rgw_build_policies(RGWRados *store, struct req_state *s, bool only_bucket, bool prefetch_data)
{
  int ret = 0;
  rgw_obj_key obj;
  RGWUserInfo bucket_owner_info;
  RGWObjectCtx& obj_ctx = *static_cast<RGWObjectCtx *>(s->obj_ctx);

  string bi = s->info.args.get(RGW_SYS_PARAM_PREFIX "bucket-instance");
  if (!bi.empty()) {
    int shard_id;
    ret = rgw_bucket_parse_bucket_instance(bi, &s->bucket_instance_id, &shard_id);
    if (ret < 0) {
      return ret;
    }
  }

  if(s->dialect.compare("s3") == 0) {
    s->bucket_acl = new RGWAccessControlPolicy_S3(s->cct);
  } else if(s->dialect.compare("swift")  == 0) {
    //s->bucket_acl = new RGWAccessControlPolicy_SWIFT(s->cct);
  } else {
    s->bucket_acl = new RGWAccessControlPolicy(s->cct);
  }

  if (s->copy_source) { /* check if copy source is within the current domain */
    const char *src = s->copy_source;
    if (*src == '/')
      ++src;
    string copy_source_str(src);

    int pos = copy_source_str.find('/');
    if (pos > 0)
      copy_source_str = copy_source_str.substr(0, pos);

    RGWBucketInfo source_info;

    ret = store->get_bucket_info(obj_ctx, copy_source_str, source_info, NULL);
    if (ret == 0) {//bucket is exist ????
      string& region = source_info.region;
//      s->local_source = store->region.equals(region);
    }
  }

  if (!s->bucket_name_str.empty()) {
    s->bucket_exists = true;
    if (s->bucket_instance_id.empty()) {
      ret = store->get_bucket_info(obj_ctx, s->bucket_name_str, s->bucket_info, NULL, &s->bucket_attrs);
    } else {
//      ret = store->get_bucket_instance_info(obj_ctx, s->bucket_instance_id, s->bucket_info, NULL, &s->bucket_attrs);
    }
    if (ret < 0) {//is not exist ....
      if (ret != -ENOENT) {
        ldout(s->cct, 0) << "NOTICE: couldn't get bucket from bucket_name (name=" << s->bucket_name_str << ")" << dendl;
        return ret;
      }
      s->bucket_exists = false;
    }
    s->bucket = s->bucket_info.bucket;

    if (s->bucket_exists) {
      rgw_obj_key no_obj;
      ret = read_policy(store, s, s->bucket_info, s->bucket_attrs, s->bucket_acl, s->bucket, no_obj);
    } else {
      s->bucket_acl->create_default(s->user.user_id, s->user.display_name);
      ret = -ERR_NO_SUCH_BUCKET;
    }

    s->bucket_owner = s->bucket_acl->get_owner();

    string& region = s->bucket_info.region;
//    map<string, RGWRegion>::iterator dest_region = store->region_map.regions.find(region);
//    if (dest_region != store->region_map.regions.end() && !dest_region->second.endpoints.empty()) {
//      s->region_endpoint = dest_region->second.endpoints.front();
//    }
#if 0    //redirect to other region
    if (s->bucket_exists /*&& !store->region.equals(region)*/) {
        //s->local_source = true;//add by sean or it return ERR_PERMANENT_REDIRECT
//      ldout(s->cct, 0) << "NOTICE: request for data in a different region (" << region << " != " << store->region.name << ")" << dendl;
      /* we now need to make sure that the operation actually requires copy source, that is
       * it's a copy operation
       */
      if (/*store->region.is_master && */s->op == OP_DELETE && s->system_request) {
        /*If the operation is delete and if this is the master, don't redirect*/
      } else if (!s->local_source ||
          (s->op != OP_PUT && s->op != OP_COPY) ||
          s->object.empty()) {
        return -ERR_PERMANENT_REDIRECT;
      }
    }
#endif
  }

  /* we're passed only_bucket = true when we specifically need the bucket's
     acls, that happens on write operations */
  if (!only_bucket && !s->object.empty()) {
    if (!s->bucket_exists) {
      return -ERR_NO_SUCH_BUCKET;
    }
    s->object_acl = new RGWAccessControlPolicy(s->cct);

    rgw_obj obj(s->bucket, s->object);
    store->set_atomic(s->obj_ctx, obj);
    if (prefetch_data) {
      store->set_prefetch_data(s->obj_ctx, obj);
    }
    ret = read_policy(store, s, s->bucket_info, s->bucket_attrs, s->object_acl, s->bucket, s->object);
  }

  return ret;
}

int RGWHandler::do_read_permissions(RGWOp *op, bool only_bucket)
{
  int ret = rgw_build_policies(store, s, only_bucket, op->prefetch_data());
#if 0
  if (ret < 0) {
    ldout(s->cct, 10) << "read_permissions on " << s->bucket << ":" <<s->object << " only_bucket=" << only_bucket << " ret=" << ret << dendl;
    if (ret == -ENODATA)
      ret = -EACCES;
  }
#endif
  return ret;
}


RGWOp *RGWHandler::get_op(RGWRados *store)
{
  RGWOp *op;
  switch (s->op) {
   case OP_GET:
     op = op_get();
     break;
   case OP_PUT:
     op = op_put();
     break;
   case OP_DELETE:
     op = op_delete();
     break;
   case OP_HEAD:
     op = op_head();
     break;
   case OP_POST:
     op = op_post();
     break;
   case OP_COPY:
     op = op_copy();
     break;
   case OP_OPTIONS:
     op = op_options();
     break;
   default:
     return NULL;
  }

  if (op) {
    op->init(store, s, this);
  }
  return op;
}

void RGWHandler::put_op(RGWOp *op)
{
  delete op;
}

int RGWListBuckets::verify_permission()
{
  return 0;
}

void RGWListBuckets::execute()
{
  bool done;
  bool started = false;
  uint64_t total_count = 0;

  uint64_t max_buckets = G.rgw_list_buckets_max_chunk;//s->cct->_conf->rgw_list_buckets_max_chunk;

  ret = get_params();
  if (ret < 0) {
    goto send_end;
  }

  if (supports_account_metadata()) {
//    ret = rgw_get_user_attrs_by_uid(store, s->user.user_id, attrs);
    if (ret < 0) {
      goto send_end;
    }
  }

  do {
    RGWUserBuckets buckets;
    uint64_t read_count;
    if (limit >= 0) {
      read_count = min(limit - total_count, (uint64_t)max_buckets);
    } else {
      read_count = max_buckets;
    }

    ret = rgw_read_user_buckets(store, s->user.user_id, buckets,
                                marker, read_count, should_get_stats(), 0);
//    RGWBucketEnt test;
//    test.bucket.name = "bucket1";
//    buckets.add(test);
//    read_count=2;
    
    if (ret < 0) {
      /* hmm.. something wrong here.. the user was authenticated, so it
         should exist */
//      ldout(s->cct, 10) << "WARNING: failed on rgw_get_user_buckets uid=" << s->user.user_id << dendl;
      break;
    }
    map<string, RGWBucketEnt>& m = buckets.get_buckets();
    map<string, RGWBucketEnt>::iterator iter;
    for (iter = m.begin(); iter != m.end(); ++iter) {
      RGWBucketEnt& bucket = iter->second;
      buckets_size += bucket.size;
      buckets_size_rounded += bucket.size_rounded;
      buckets_objcount += bucket.count;

      marker = iter->first;
    }
    buckets_count += m.size();
    total_count += m.size();

    done = (m.size() < read_count || (limit >= 0 && total_count >= (uint64_t)limit));

    if (!started) {
      send_response_begin(buckets.count() > 0);
      started = true;
    }

    if (!m.empty()) {
      send_response_data(buckets);

      map<string, RGWBucketEnt>::reverse_iterator riter = m.rbegin();
      marker = riter->first;
    }
  } while (!done);

send_end:
  if (!started) {
    send_response_begin(false);
  }
  send_response_end();
}

int RGWListBucket::verify_permission()
{
  if (!verify_bucket_permission(s, RGW_PERM_READ))
    return -EACCES;

  return 0;
}

static void rgw_bucket_object_pre_exec(struct req_state *s)
{
  if (s->expect_cont)
    dump_continue(s);

  dump_bucket_from_state(s);
}

void RGWListBucket::pre_exec()
{
  rgw_bucket_object_pre_exec(s);
}
int RGWListBucket::parse_max_keys()
{
  if (!max_keys.empty()) {
    char *endptr;
    max = strtol(max_keys.c_str(), &endptr, 10);
    if (endptr) {
      while (*endptr && isspace(*endptr)) // ignore white space
        endptr++;
      if (*endptr) {
        return -EINVAL;
      }
    }
  } else {
    max = default_max;
  }

  return 0;
}
void RGWListBucket::execute()
{
  ret = get_params();
  if (ret < 0)
    return;

  if (need_container_stats()) {//just swift need
    map<string, RGWBucketEnt> m;
    m[s->bucket.name] = RGWBucketEnt();
    m.begin()->second.bucket = s->bucket;
    ret = 0;//store->update_containers_stats(m);
    if (ret > 0) {
      bucket = m.begin()->second;
    }
  }
#if 0  // is ok
  RGWObjEnt ent;
  rgw_obj_key aaa("helloo.txt");
  ent.key =  aaa;
  ent.ns = "ns"; 
  ent.owner = "hwttt";
  ent.size = 10000;
  ent.etag = "etag";
  ent.tag = "tagg";
  ent.owner_display_name = "htw display";
  ent.content_type = ".jpg";
  objs.push_back(ent);
  ent.owner = "aaaaaas";
  ent.size = 100;
  objs.push_back(ent);
#endif
  RGWRados::Bucket target(store, s->bucket);
  RGWRados::Bucket::List list_op(&target);
#if 1
  list_op.params.prefix = prefix;
  list_op.params.delim = delimiter;
  list_op.params.marker = marker;
  list_op.params.end_marker = end_marker;
  list_op.params.list_versions = list_versions;

  ret = list_op.list_objects(max, &objs, &common_prefixes, &is_truncated);
  if (ret >= 0 && !delimiter.empty()) {
    next_marker = list_op.get_next_marker();
  }
#endif
}

/**
 * Generate the CORS header response
 *
 * This is described in the CORS standard, section 6.2.
 */
bool RGWOp::generate_cors_headers(string& origin, string& method, string& headers, string& exp_headers, unsigned *max_age)
{
  /* CORS 6.2.1. */
  const char *orig = s->info.env->get("HTTP_ORIGIN");
  if (!orig) {
    return false;
  }

  /* Custom: */
  origin = orig;
  int ret = read_bucket_cors();
  if (ret < 0) {
    return false;
  }

  if (!cors_exist) {
    dout(2) << "No CORS configuration set yet for this bucket" << dendl;
    return false;
  }

  /* CORS 6.2.2. */
//  RGWCORSRule *rule = bucket_cors.host_name_rule(orig);
//  if (!rule)
//    return false;

  /* CORS 6.2.3. */
  const char *req_meth = s->info.env->get("HTTP_ACCESS_CONTROL_REQUEST_METHOD");
  if (!req_meth) {
    req_meth = s->info.method;
  }

  if (req_meth) {
    method = req_meth;
    /* CORS 6.2.5. */
//    if (!validate_cors_rule_method(rule, req_meth)) {
//     return false;
//    }
  }

  /* CORS 6.2.4. */
  const char *req_hdrs = s->info.env->get("HTTP_ACCESS_CONTROL_REQUEST_HEADERS");

  /* CORS 6.2.6. */
//  get_cors_response_headers(rule, req_hdrs, headers, exp_headers, max_age);

  return true;
}

class RGWGetObj_CB : public RGWGetDataCB
{
  RGWGetObj *op;
public:
  RGWGetObj_CB(RGWGetObj *_op) : op(_op) {}
  virtual ~RGWGetObj_CB() {}

  int handle_data(bufferlist& bl, off_t bl_ofs, off_t bl_len) {
    return op->get_data_cb(bl, bl_ofs, bl_len);
  }
};

int RGWGetObj::init_common()
{
  if (range_str) {
    /* range parsed error when prefetch*/
    if (!range_parsed) {
      int r = parse_range(range_str, ofs, end, &partial_content);
      if (r < 0)
        return r;
    }
  }
  if (if_mod) {
    if (parse_time(if_mod, &mod_time) < 0)
      return -EINVAL;
    mod_ptr = &mod_time;
  }

  if (if_unmod) {
    if (parse_time(if_unmod, &unmod_time) < 0)
      return -EINVAL;
    unmod_ptr = &unmod_time;
  }

  return 0;
}

bool RGWGetObj::prefetch_data()
{
  /* HEAD request, stop prefetch*/
  if (!get_data) {
    return false;
  }

  bool prefetch_first_chunk = true;
  range_str = s->info.env->get("HTTP_RANGE");

  if(range_str) {
    int r = parse_range(range_str, ofs, end, &partial_content);
    /* error on parsing the range, stop prefetch and will fail in execte() */
    if (r < 0) {
      range_parsed = false;
      return false;
    } else {
      range_parsed = true;
    }
    /* range get goes to shadown objects, stop prefetch */
//    if (ofs >= s->cct->_conf->rgw_max_chunk_size) {
//      prefetch_first_chunk = false;
//    }
  }

  return get_data && prefetch_first_chunk;
}
void RGWGetObj::pre_exec()
{
  rgw_bucket_object_pre_exec(s);
}

void RGWGetObj::execute()
{
  uint64_t lenaa = 0;                   
  string full_path="";  
  struct stat st;
  int64_t left;
  librados::ObjectReadOperation test;  
#define BLOCK_SIZE          (4*1024)            /*  */
  char buf[BLOCK_SIZE+1];
  int fd = -1;
  size_t off = 0;
  int result = 0;
  unsigned char m[CEPH_CRYPTO_MD5_DIGESTSIZE];
  char calc_md5[CEPH_CRYPTO_MD5_DIGESTSIZE * 2 + 1];  
  MD5 hash;
  char md5_buf[512]={0};
  string md5_val;
  size_t cur_ofs = 0;

  utime_t start_time = s->time;
  bufferlist bl;
  gc_invalidate_time = ceph_clock_now(s->cct);
  gc_invalidate_time += 10;//(s->cct->_conf->rgw_gc_obj_min_wait / 2);

  RGWGetObj_CB cb(this);

  map<string, bufferlist>::iterator attr_iter;

//  perfcounter->inc(l_rgw_get);
  int64_t new_ofs, new_end;

  RGWRados::Object op_target(store, s->bucket_info, *static_cast<RGWObjectCtx *>(s->obj_ctx), obj);
  RGWRados::Object::Read read_op(&op_target);

  ret = get_params();
  if (ret < 0)
    goto done_err;

  ret = init_common();
  if (ret < 0)
    goto done_err;

  new_ofs = ofs;
  new_end = end;

  full_path += G.buckets_root + string("/") + s->bucket.name +string("/") + s->object.name;
  ::stat(full_path.c_str(), &st);

  read_op.conds.mod_ptr = mod_ptr;
  read_op.conds.unmod_ptr = unmod_ptr;
  read_op.conds.if_match = if_match;
  read_op.conds.if_nomatch = if_nomatch;
  read_op.params.attrs = &attrs;
  read_op.params.lastmod = &lastmod;
  read_op.params.read_size = &total_len;
  read_op.params.obj_size = &s->obj_size;
  read_op.params.perr = &s->err;

  *read_op.params.read_size = st.st_size;
  *read_op.params.obj_size =  st.st_size;
#if 0
  ret = read_op.prepare(&new_ofs, &new_end);
  if (ret < 0)
    goto done_err;

  attr_iter = attrs.find(RGW_ATTR_USER_MANIFEST);
  if (attr_iter != attrs.end() && !skip_manifest) {
      s->obj_size = total_len;
//    ret = handle_user_manifest(attr_iter->second.c_str());
    if (ret < 0) {
      ldout(s->cct, 0) << "ERROR: failed to handle user manifest ret=" << ret << dendl;
    }
    return;
  }

  /* Check whether the object has expired. Swift API documentation
   * stands that we should return 404 Not Found in such case. */
  if (need_object_expiration() && object_is_expired(attrs)) {
    ret = -ENOENT;
    goto done_err;
  }

  ofs = new_ofs;
  end = new_end;

  start = ofs;

  if (!get_data || ofs > end)
    goto done_err;

//  perfcounter->inc(l_rgw_get_b, end - ofs);

  ret = read_op.iterate(ofs, end, &cb);

//  perfcounter->tinc(l_rgw_get_lat,
//                   (ceph_clock_now(s->cct) - start_time));

  if (ret < 0) {
    goto done_err;
  }
#else
 /* :TODO:2015/12/29 17:46:41:hwt:  */
//  lenaa = st.st_size;
  if (ofs != 0 || end != -1)
  {
    left = end-ofs;
  }
  else
  {
//    end = st.st_size;
    new_end = st.st_size;
    left = st.st_size;
  }

  if ((fd = ::open(full_path.c_str(), O_RDONLY)) < 0)
  {
      result = -1;
      ret = -ERR_NOT_FOUND;//ERR_PRECONDITION_FAILED;
      goto done_err;
  }
  if (ofs != 0 || end != -1)
  {
      bl.pread_fd(fd, left, ofs);
      hash.Update((const byte *)bl.c_str(), bl.length());
      bl.clear();
      hash.Final(m);
      buf_to_hex(m, CEPH_CRYPTO_MD5_DIGESTSIZE, calc_md5);
      bl.append(calc_md5, strlen(calc_md5)+1);

  }
  else
  {
      sprintf(md5_buf, "md5sum %s", full_path.c_str());
      md5_val = shell_execute(md5_buf);
      sscanf(md5_val.c_str(), "%s", md5_buf);
      printf("md5_val.c_str() [%s] md5_buf [%s] \n", md5_val.c_str(), md5_buf);
      bl.clear();
      bl.append(md5_buf, strlen(md5_buf)+1);
  }

  attrs.insert(pair<string, bufferlist>(RGW_ATTR_ETAG, bl));
  bl.clear();
  send_response_data(bl, 0, bl.length());


  
  while (left > 0)
  {
#if 1
      ssize_t len = G.rgw_max_chunk_size > left ? left : G.rgw_max_chunk_size;
      int retv;
      if (ofs != 0 || end != -1)
      {

          retv = bl.pread_fd(fd, len, ofs + cur_ofs);
          cur_ofs += len; 
      }
      else
      {
          retv = bl.read_fd(fd, len);
      }
      if (retv != len)
      {
          goto done_err;
      }
#else
      if ((result = ::pread(fd, buf, BLOCK_SIZE, off)) < 0)
      {
          goto done_err;
      }
      bl.append(buf, BLOCK_SIZE);
#endif
//      hash.Update((const byte *)bl.c_str(), bl.length());
      send_response_data(bl, 0, bl.length());
      bl.clear();
      left -= G.rgw_max_chunk_size;
  }  
//  hash.Final(m);
//  buf_to_hex(m, CEPH_CRYPTO_MD5_DIGESTSIZE, calc_md5);  
//  bl.clear();
//  bl.append(calc_md5, strlen(calc_md5));
//  attrs.insert(pair<string, bufferlist>(RGW_ATTR_ETAG, bl));
  bl.clear();
  
//  bl.append("hello this is test", strlen("hello this is test"));
 /* :TODO:End---  */
#endif
  send_response_data(bl, 0, /*sizeof("abcdefgaaaa")*/0);
  return;

done_err:
  send_response_data_error();
}

int RGWGetObj::verify_permission()
{

  obj = rgw_obj(s->bucket, s->object);
  store->set_atomic(s->obj_ctx, obj);
  if (get_data)
    store->set_prefetch_data(s->obj_ctx, obj);
#if 0
  if (!verify_object_permission(s, RGW_PERM_READ))
    return -EACCES;
#endif
  return 0;
}
int RGWGetObj::get_data_cb(bufferlist& bl, off_t bl_ofs, off_t bl_len)
{
  /* garbage collection related handling */
  utime_t start_time = ceph_clock_now(s->cct);
  if (start_time > gc_invalidate_time) {
    int r = 0;//store->defer_gc(s->obj_ctx, obj);
    if (r < 0) {
      dout(0) << "WARNING: could not defer gc entry for obj" << dendl;
    }
    gc_invalidate_time = start_time;
    gc_invalidate_time += 10;//(s->cct->_conf->rgw_gc_obj_min_wait / 2);
  }
  return send_response_data(bl, bl_ofs, bl_len);
}

void RGWCreateBucket::pre_exec()
{
  rgw_bucket_object_pre_exec(s);
}

void RGWCreateBucket::execute()
{
  RGWAccessControlPolicy old_policy(s->cct);
  map<string, bufferlist> attrs;
  bufferlist aclbl;
  bufferlist corsbl;
  bool existed;
  rgw_obj obj(s->bucket/*store->zone.domain_root*/, s->bucket_name_str);
  obj_version objv, *pobjv = NULL;

  ret = get_params();
  if (ret < 0)
    return;

//  if (!store->region.is_master &&
//      store->region.api_name != location_constraint) {
//    ldout(s->cct, 0) << "location constraint (" << location_constraint << ") doesn't match region" << " (" << store->region.api_name << ")" << dendl;
//    ret = -EINVAL;
//    return;
//  }

  /* we need to make sure we read bucket info, it's not read before for this specific request */
  RGWObjectCtx& obj_ctx = *static_cast<RGWObjectCtx *>(s->obj_ctx);
  ret = store->get_bucket_info(obj_ctx, s->bucket_name_str, s->bucket_info, NULL, &s->bucket_attrs);
  if (ret < 0 && ret != -ENOENT)
    return;
  s->bucket_exists = (ret != -ENOENT);

//  s->bucket_owner.set_id(s->user.user_id);
//  s->bucket_owner.set_name(s->user.display_name);
  if (s->bucket_exists) {
    int r = 0;//get_policy_from_attr(s->cct, store, s->obj_ctx, s->bucket_info, s->bucket_attrs,
//                                 &old_policy, obj);
    if (r >= 0)  {
      if (old_policy.get_owner().get_id().compare(s->user.user_id) != 0) {
        ret = -EEXIST;
        return;
      }
    }
  }

  RGWBucketInfo master_info;
  rgw_bucket *pmaster_bucket;
  time_t creation_time;

  if (0/* !store->region.is_master */) {
#if 0
    JSONParser jp;
    ret = forward_request_to_master(s, NULL, store, in_data, &jp);
    if (ret < 0)
      return;

    JSONDecoder::decode_json("entry_point_object_ver", ep_objv, &jp);
    JSONDecoder::decode_json("object_ver", objv, &jp);
    JSONDecoder::decode_json("bucket_info", master_info, &jp);
    ldout(s->cct, 20) << "parsed: objv.tag=" << objv.tag << " objv.ver=" << objv.ver << dendl;
    ldout(s->cct, 20) << "got creation time: << " << master_info.creation_time << dendl;
    pmaster_bucket= &master_info.bucket;
    creation_time = master_info.creation_time;
    pobjv = &objv;
#endif
  } else {
    pmaster_bucket = NULL;
    creation_time = 0;
  }

  string region_name;

  if (s->system_request) {
    region_name = s->info.args.get(RGW_SYS_PARAM_PREFIX "region");
    if (region_name.empty()) {
//      region_name = store->region.name;
    }
  } else {
//    region_name = store->region.name;
  }

  if (s->bucket_exists) {
    string selected_placement_rule;
    rgw_bucket bucket;
//    ret = store->select_bucket_placement(s->user, region_name, placement_rule, s->bucket_name_str, bucket, &selected_placement_rule);
    if (selected_placement_rule != s->bucket_info.placement_rule) {
      ret = -EEXIST;
      return;
    }
  }

  policy.encode(aclbl);

  attrs[RGW_ATTR_ACL] = aclbl;

  if (has_cors) {
//    cors_config.encode(corsbl);
    attrs[RGW_ATTR_CORS] = corsbl;
  }
  s->bucket.name = s->bucket_name_str;
  ret = store->create_bucket(s->user, s->bucket, region_name, placement_rule, attrs, info, pobjv,
                             &ep_objv, creation_time, pmaster_bucket, true);//并不真实 创建：ceph:创建存储池()不关联？
  /* continue if EEXIST and create_bucket will fail below.  this way we can recover
   * from a partial create by retrying it. */
  ldout(s->cct, 20) << "rgw_create_bucket returned ret=" << ret << " bucket=" << s->bucket << dendl;

  if (ret && ret != -EEXIST)
    return;

  existed = (ret == -EEXIST);

  if (existed) {
    /* bucket already existed, might have raced with another bucket creation, or
     * might be partial bucket creation that never completed. Read existing bucket
     * info, verify that the reported bucket owner is the current user.
     * If all is ok then update the user's list of buckets.
     * Otherwise inform client about a name conflict.
     */
    if (info.owner.compare(s->user.user_id) != 0) {
      ret = -EEXIST;
      return;
    }
    s->bucket = info.bucket;
  }
#if 0
  ret = rgw_link_bucket(store, s->user.user_id, s->bucket, info.creation_time, false);//关联bucket 创建bucket(文件夹，卷)
  if (ret && !existed && ret != -EEXIST) {  /* if it exists (or previously existed), don't remove it! */
    ret = rgw_unlink_bucket(store, s->user.user_id, s->bucket.name);
    if (ret < 0) {
      ldout(s->cct, 0) << "WARNING: failed to unlink bucket: ret=" << ret << dendl;
    }
  } else if (ret == -EEXIST || (ret == 0 && existed)) {
    ret = -ERR_BUCKET_EXISTS;
  }
#endif
}

int RGWDeleteBucket::verify_permission()
{
  if (!verify_bucket_permission(s, RGW_PERM_WRITE))
    return -EACCES;

  return 0;
}
void RGWDeleteBucket::pre_exec()
{
  rgw_bucket_object_pre_exec(s);
}

void RGWDeleteBucket::execute()
{
  ret = -EINVAL;

  if (s->bucket_name_str.empty())
    return;

  RGWObjVersionTracker ot;
  ot.read_version = s->bucket_info.ep_objv;

  if (s->system_request) {
    string tag = s->info.args.get(RGW_SYS_PARAM_PREFIX "tag");
    string ver_str = s->info.args.get(RGW_SYS_PARAM_PREFIX "ver");
    if (!tag.empty()) {
      ot.read_version.tag = tag;
      uint64_t ver;
      string err;
      ver = strict_strtol(ver_str.c_str(), 10, &err);
      if (!err.empty()) {
        ldout(s->cct, 0) << "failed to parse ver param" << dendl;
        ret = -EINVAL;
        return;
      }
      ot.read_version.ver = ver;
    }
  }
#if 0
  ret = store->delete_bucket(s->bucket, ot);

  if (ret == 0) {
    ret = rgw_unlink_bucket(store, s->user.user_id, s->bucket.name, false);
    if (ret < 0) {
      ldout(s->cct, 0) << "WARNING: failed to unlink bucket: ret=" << ret << dendl;
    }
  }

  if (ret < 0) {
    return;
  }

  if (!store->region.is_master) {
    bufferlist in_data;
    JSONParser jp;
    ret = forward_request_to_master(s, &ot.read_version, store, in_data, &jp);
    if (ret < 0) {
      if (ret == -ENOENT) { /* adjust error,
                               we want to return with NoSuchBucket and not NoSuchKey */
        ret = -ERR_NO_SUCH_BUCKET;
      }
      return;
    }
  }
#endif
  char cmd_buf[512];

  sprintf(cmd_buf, "[ -d %s/%s ]", G.buckets_root.c_str(), s->bucket_name_str.c_str());
  if ((ret = shell_simple(cmd_buf))!=0)
  {
    ret = -ERR_NO_SUCH_BUCKET;
    return;
  }
#ifdef FICS
#if 1
  int con_fd;
  char del_buf[256];
  con_fd = post_msg(CGW_MSG_DEL_VOLUME, s->bucket_name_str.c_str(), s->bucket_name_str.size()+1, false);
  if (1 == recv_msg(con_fd, del_buf, true))
  {
      //not found
      dout(0) << "ERROR: del vol fail" << dendl;
      ret = ERR_INTERNAL_ERROR;
      return;
  }

#endif
#else
  sprintf(cmd_buf, "rm -rf %s/%s", G.buckets_root.c_str(), s->bucket_name_str.c_str());
  if ((ret = shell_simple(cmd_buf))!=0)
  {
    ret = ERR_INTERNAL_ERROR;
    return;
  }
#endif
}
void RGWPutObj::pre_exec()
{
  rgw_bucket_object_pre_exec(s);
}
void RGWPutObj::execute()
{
//  RGWPutObjProcessor *processor = NULL;
  char supplied_md5_bin[CEPH_CRYPTO_MD5_DIGESTSIZE + 1];
  char supplied_md5[CEPH_CRYPTO_MD5_DIGESTSIZE * 2 + 1];
  char calc_md5[CEPH_CRYPTO_MD5_DIGESTSIZE * 2 + 1];
  unsigned char m[CEPH_CRYPTO_MD5_DIGESTSIZE];
  MD5 hash;
  bufferlist bl, aclbl;
  map<string, bufferlist> attrs;
  int len;
  map<string, string>::iterator iter;
  bool multipart = s->info.args.exists("uploadId");

  bool need_calc_md5 = (obj_manifest == NULL);
  string full_path=""; 
  int fd = -1;
  int result = -1;
#define BLOCK_SIZE          (4*1024)            /*  */
  char buf[BLOCK_SIZE+1];
  uid_t old_uid = 0;
  int id;
  int flag;

//  perfcounter->inc(l_rgw_put);
  ret = -EINVAL;
  if (s->object.empty()) {
    goto done;
  }

  ret = get_params();
  if (ret < 0) {
    ldout(s->cct, 20) << "get_params() returned ret=" << ret << dendl;
    goto done;
  }

  ret = 0;// get_system_versioning_params(s, &olh_epoch, &version_id);
  if (ret < 0) {
    ldout(s->cct, 20) << "get_system_versioning_params() returned ret=" \
        << ret << dendl;
    goto done;
  }

  if (supplied_md5_b64) {
    need_calc_md5 = true;

    ldout(s->cct, 15) << "supplied_md5_b64=" << supplied_md5_b64 << dendl;
    ret = ceph_unarmor(supplied_md5_bin, &supplied_md5_bin[CEPH_CRYPTO_MD5_DIGESTSIZE + 1],
                       supplied_md5_b64, supplied_md5_b64 + strlen(supplied_md5_b64));
    ldout(s->cct, 15) << "ceph_armor ret=" << ret << dendl;
    if (ret != CEPH_CRYPTO_MD5_DIGESTSIZE) {
      ret = -ERR_INVALID_DIGEST;
      goto done;
    }

    buf_to_hex((const unsigned char *)supplied_md5_bin, CEPH_CRYPTO_MD5_DIGESTSIZE, supplied_md5);
    ldout(s->cct, 15) << "supplied_md5=" << supplied_md5 << dendl;
  }

  if (!chunked_upload) { /* with chunked upload we don't know how big is the upload.
                            we also check sizes at the end anyway */
    ret = 0;//store->check_quota(s->bucket_owner.get_id(), s->bucket,
//                             user_quota, bucket_quota, s->content_length);
    if (ret < 0) {
      ldout(s->cct, 20) << "check_quota() returned ret=" << ret << dendl;
      goto done;
    }
  }

  if (supplied_etag) {
    strncpy(supplied_md5, supplied_etag, sizeof(supplied_md5) - 1);
    supplied_md5[sizeof(supplied_md5) - 1] = '\0';
  }

//  processor = select_processor(*static_cast<RGWObjectCtx *>(s->obj_ctx), &multipart);

  ret = 0;//processor->prepare(store, NULL);
  if (ret < 0) {
    ldout(s->cct, 20) << "processor->prepare() returned ret=" << ret << dendl;
    goto done;
  }
//  name = get_cur_username();

//  new_uid = getuid(name.c_str());
  old_uid = drop_privs(s->user.auid);

  full_path += G.buckets_root + string("/") + s->bucket.name +string("/") + s->object.name;
  flag =  O_RDWR|O_CREAT;
  if (multipart)
  {
      flag |= O_APPEND;
  }
  else
  {
      flag |= O_TRUNC;
  }
  if ((fd = ::open(full_path.c_str(), flag)) < 0)
  {
      result = -1;
      goto done;//_err;
  }
  chmod(full_path.c_str(), 07777);
  
  do {
    bufferlist data;
    len = get_data(data);
    if (len < 0) {
      ret = len;
      goto done;
    }
    if (!len)
      break;

    /* do we need this operation to be synchronous? if we're dealing with an object with immutable
     * head, e.g., multipart object we need to make sure we're the first one writing to this object
     */
    bool need_to_wait = (ofs == 0) && multipart;

    bufferlist orig_data;

    if (need_to_wait) {
      orig_data = data;
    }

#if 0
    ret = put_data_and_throttle(processor, data, ofs, (need_calc_md5 ? &hash : NULL), need_to_wait);

    if (ret < 0) {
      if (!need_to_wait || ret != -EEXIST) {
        ldout(s->cct, 20) << "processor->thottle_data() returned ret=" << ret << dendl;
        goto done;
      }

      ldout(s->cct, 5) << "NOTICE: processor->throttle_data() returned -EEXIST, need to restart write" << dendl;

      /* restore original data */
      data.swap(orig_data);

      /* restart processing with different oid suffix */

      dispose_processor(processor);
      processor = select_processor(*static_cast<RGWObjectCtx *>(s->obj_ctx), &multipart);

      string oid_rand;
      char buf[33];
      gen_rand_alphanumeric(store->ctx(), buf, sizeof(buf) - 1);
      oid_rand.append(buf);

      ret = processor->prepare(store, &oid_rand);
      if (ret < 0) {
        ldout(s->cct, 0) << "ERROR: processor->prepare() returned " << ret << dendl;
        goto done;
      }

      ret = put_data_and_throttle(processor, data, ofs, NULL, false);
      if (ret < 0) {
        goto done;
      }
    }
#else
#if 0
    if ((result = ::pwrite(fd, data.c_str(), len, ofs)) < 0)
    {
        goto done;
    }
#else
    if ((result = ::write(fd, data.c_str(), len)) < 0)
    {
        goto done;
    }
#endif
    hash.Update((const byte *)data.c_str(), data.length());
    
    data.clear();
#endif
    
    ofs += len;
  } while (len > 0);
#if 0
  if (!chunked_upload && ofs != s->content_length) {
    ret = -ERR_REQUEST_TIMEOUT;
    goto done;
  }
#endif
  s->obj_size = ofs;
//  perfcounter->inc(l_rgw_put_b, s->obj_size);

  ret = store->check_quota("bwcpn"/*s->bucket_owner.get_id()*/, s->bucket,
                           user_quota, bucket_quota, s->obj_size);
  if (ret < 0) {
    ldout(s->cct, 20) << "second check_quota() returned ret=" << ret << dendl;
    goto done;
  }

  if (need_calc_md5) {
#if 1
//      processor->complete_hash(&hash);
    hash.Final(m);

    buf_to_hex(m, CEPH_CRYPTO_MD5_DIGESTSIZE, calc_md5);
    etag = calc_md5;

//    if (supplied_md5_b64 && strcmp(calc_md5, supplied_md5)) {
//      ret = -ERR_BAD_DIGEST;
//      goto done;
//    }
#endif
  }

//  policy.encode(aclbl);

  attrs[RGW_ATTR_ACL] = aclbl;
#if 0
  if (obj_manifest) {
    bufferlist manifest_bl;
    string manifest_obj_prefix;
    string manifest_bucket;

    char etag_buf[CEPH_CRYPTO_MD5_DIGESTSIZE];
    char etag_buf_str[CEPH_CRYPTO_MD5_DIGESTSIZE * 2 + 16];

    manifest_bl.append(obj_manifest, strlen(obj_manifest) + 1);
    attrs[RGW_ATTR_USER_MANIFEST] = manifest_bl;
    user_manifest_parts_hash = &hash;
    string prefix_str = obj_manifest;
    int pos = prefix_str.find('/');
    if (pos < 0) {
      ldout(s->cct, 0) << "bad user manifest, missing slash separator: " << obj_manifest << dendl;
      goto done;
    }

    manifest_bucket = prefix_str.substr(0, pos);
    manifest_obj_prefix = prefix_str.substr(pos + 1);

    hash.Final((byte *)etag_buf);
    buf_to_hex((const unsigned char *)etag_buf, CEPH_CRYPTO_MD5_DIGESTSIZE, etag_buf_str);

    ldout(s->cct, 0) << __func__ << ": calculated md5 for user manifest: " << etag_buf_str << dendl;

    etag = etag_buf_str;
  }
  if (supplied_etag && etag.compare(supplied_etag) != 0) {
    ret = -ERR_UNPROCESSABLE_ENTITY;
    goto done;
  }
  bl.append(etag.c_str(), etag.size() + 1);
  attrs[RGW_ATTR_ETAG] = bl;

  for (iter = s->generic_attrs.begin(); iter != s->generic_attrs.end(); ++iter) {
    bufferlist& attrbl = attrs[iter->first];
    const string& val = iter->second;
    attrbl.append(val.c_str(), val.size() + 1);
  }

  rgw_get_request_metadata(s->cct, s->info, attrs);
  encode_delete_at_attr(delete_at, attrs);

  ret = processor->complete(etag, &mtime, 0, attrs, delete_at, if_match, if_nomatch);
#endif
done:
//update stat
  sys_info info;
  string stat_path = G.buckets_root + string("/") + s->bucket.name + string("/") + string(PROC_BUCKET_PATH) + string("/") + string(PROC_STAT);
  info.set_file(stat_path);
  parse_conf(stat_path.c_str(), &info, ":",(FUNC)(&info.get_params));
  info.num_entries += 1;
  info.total_size += s->obj_size;
  sys_info::set_params(&info, sys_info::BUCNET_STAT, NULL, NULL);
  id = restore_privs(old_uid);
//  dispose_processor(processor);
//  perfcounter->tinc(l_rgw_put_lat,
//                   (ceph_clock_now(s->cct) - s->time));
}
int RGWInitMultipart::verify_permission()
{
  if (!verify_bucket_permission(s, RGW_PERM_WRITE))
    return -EACCES;

  return 0;
}

void RGWInitMultipart::pre_exec()
{
  rgw_bucket_object_pre_exec(s);
}

void RGWInitMultipart::execute()
{
  bufferlist aclbl;
  map<string, bufferlist> attrs;
  rgw_obj obj;
  map<string, string>::iterator iter;

  if (get_params() < 0)
    return;
  ret = -EINVAL;
  if (s->object.empty())
    return;

  policy.encode(aclbl);

  attrs[RGW_ATTR_ACL] = aclbl;

  for (iter = s->generic_attrs.begin(); iter != s->generic_attrs.end(); ++iter) {
    bufferlist& attrbl = attrs[iter->first];
    const string& val = iter->second;
    attrbl.append(val.c_str(), val.size() + 1);
  }

//  rgw_get_request_metadata(s->cct, s->info, attrs);

  do {
    char buf[33];
    gen_rand_alphanumeric(s->cct, buf, sizeof(buf) - 1);
    upload_id = MULTIPART_UPLOAD_ID_PREFIX; /* v2 upload id */
    upload_id.append(buf);

    string tmp_obj_name;
    RGWMPObj mp(s->object.name, upload_id);
    tmp_obj_name = mp.get_meta();

    obj.init_ns(s->bucket, tmp_obj_name, mp_ns);
    // the meta object will be indexed with 0 size, we c
    obj.set_in_extra_data(true);
    obj.index_hash_source = s->object.name;

    RGWRados::Object op_target(store, s->bucket_info, *static_cast<RGWObjectCtx *>(s->obj_ctx), obj);
    op_target.set_versioning_disabled(true); /* no versioning for multipart meta */

//    RGWRados::Object::Write obj_op(&op_target);

//    obj_op.meta.owner = s->owner.get_id();
//    obj_op.meta.category = RGW_OBJ_CATEGORY_MULTIMETA;
//    obj_op.meta.flags = PUT_OBJ_CREATE_EXCL;

    ret = 0;//obj_op.write_meta(0, attrs);
  } while (ret == -EEXIST);
#if 0
  string full_path=""; 
  int len;
  MD5 hash;
  int fd = -1;
  int result = -1;
  full_path += G.buckets_root + string("/") + s->bucket.name +string("/") + s->object.name;
  if ((fd = ::open(full_path.c_str(), O_RDWR|O_CREAT|O_TRUNC)) < 0)
  {
      result = -1;
      goto done;//_err;
  }
  ofs = 0;
  do {
    bufferlist data;
    len = get_data(data);
    if (len < 0) {
      ret = len;
      goto done;
    }
    if (!len)
      break;
    if ((result = ::pwrite(fd, data.c_str(), len, ofs)) < 0)
    {
        goto done;
    }
    hash.Update((const byte *)data.c_str(), data.length());
    
    data.clear();
    ofs += len;
  } while (len > 0);
done:;
#endif
}
#if 0
int RGWInitMultipart::get_data(bufferlist& bl)
{
  size_t cl;
  uint64_t chunk_size = G.rgw_max_chunk_size;
  if (s->length) {
    cl = atoll(s->length) - ofs;
    if (cl > chunk_size)
      cl = chunk_size;
  } else {
    cl = chunk_size;
  }

  int len = 0;
  if (cl) {
    bufferptr bp(cl);

    int read_len; /* cio->read() expects int * */
    int r = s->cio->read(bp.c_str(), cl, &read_len);
    len = read_len;
    if (r < 0)
      return r;
    bl.append(bp, 0, len);
  }

  if ((uint64_t)ofs + len > G.rgw_max_put_size) {
    return -ERR_TOO_LARGE;
  }

  const char *supplied_md5_b64;

  if (!ofs)
    supplied_md5_b64 = s->info.env->get("HTTP_CONTENT_MD5");

  return len;
}
#endif
int RGWOp::init_quota()
{
  /* no quota enforcement for system requests */
  if (s->system_request)
    return 0;

  /* init quota related stuff */
  if (!(s->user.op_mask & RGW_OP_TYPE_MODIFY)) {
    return 0;
  }

  /* only interested in object related ops */
  if (s->object.empty()) {
    return 0;
  }

  RGWUserInfo owner_info;
  RGWUserInfo *uinfo;

  if (1/*s->user.user_id == s->bucket_owner.get_id()*/) {//用户是该桶的拥有者
    uinfo = &s->user;
  } else {
    int r = rgw_get_user_info_by_uid(store, s->bucket_info.owner, owner_info);//找出拥有者 将拥有者的配额赋值进来
    if (r < 0)
      return r;
    uinfo = &owner_info;
  }

  if (s->bucket_info.quota.enabled) {
    bucket_quota = s->bucket_info.quota;
  } else if (uinfo->bucket_quota.enabled) {
    bucket_quota = uinfo->bucket_quota;
  } else {
    ;//bucket_quota = store->region_map.bucket_quota;
  }

  if (uinfo->user_quota.enabled) {
    user_quota = uinfo->user_quota;
  } else {
    ;//user_quota = store->region_map.user_quota;
  }

  return 0;
}

void RGWDeleteObj::pre_exec()
{
  rgw_bucket_object_pre_exec(s);
}
int RGWDeleteObj::verify_permission()
{
  if (!verify_bucket_permission(s, RGW_PERM_WRITE))
    return -EACCES;

  return 0;
}
void RGWDeleteObj::execute()
{
    //using namespace librados;
    string cmd = "rm";
    uid_t old_uid = 0;
    int id;
    bufferlist bl, outbl;
    string outs;
    struct stat buf;
    string  full_path = "";
    string stat_path="";
    sys_info info;
    memset(&buf, 0, sizeof (struct stat));
    
    full_path += G.buckets_root + string("/") + s->bucket.name +string("/") + s->object.name;
    ::encode(full_path, bl);
    
    librados::Rados *rad = store->get_rados_handle();
    old_uid = drop_privs(s->user.auid);
    if (0 != ::stat(full_path.c_str(), &buf))
    {
        goto done;
    }
    ret = rad->mon_command(cmd, bl, &outbl, &outs);
  //update stat
    
    stat_path += G.buckets_root + string("/") + s->bucket.name + string("/") + string(PROC_BUCKET_PATH) + string("/") + string(PROC_STAT);
    info.set_file(stat_path);
    parse_conf(stat_path.c_str(), &info, ":",(FUNC)(&info.get_params));
    info.num_entries -= 1;
    info.total_size -= buf.st_size;//s->obj_size;
    sys_info::set_params(&info, sys_info::BUCNET_STAT, NULL, NULL);
done:
    id = restore_privs(old_uid);
    return ;
    //if (ret == -EEXIST)
    //    ret = 0;    
}
int RGWCompleteMultipart::verify_permission()
{
  if (!verify_bucket_permission(s, RGW_PERM_WRITE))
    return -EACCES;

  return 0;
}
int RGWStatBucket::verify_permission()
{
  if (!verify_bucket_permission(s, RGW_PERM_READ))
    return -EACCES;

  return 0;
}

static bool is_v2_upload_id(const string& upload_id)
{
  const char *uid = upload_id.c_str();

  return (strncmp(uid, MULTIPART_UPLOAD_ID_PREFIX, sizeof(MULTIPART_UPLOAD_ID_PREFIX) - 1) == 0) ||
         (strncmp(uid, MULTIPART_UPLOAD_ID_PREFIX_LEGACY, sizeof(MULTIPART_UPLOAD_ID_PREFIX_LEGACY) - 1) == 0);
}

static int list_multipart_parts(RGWRados *store, struct req_state *s,
                                const string& upload_id,
                                string& meta_oid, int num_parts,
                                int marker, map<uint32_t, RGWUploadPartInfo>& parts,
                                int *next_marker, bool *truncated,
                                bool assume_unsorted = false)
{
  map<string, bufferlist> parts_map;
  map<string, bufferlist>::iterator iter;
  bufferlist header;

  rgw_obj obj;
  obj.init_ns(s->bucket, meta_oid, mp_ns);
  obj.set_in_extra_data(true);

  bool sorted_omap = is_v2_upload_id(upload_id) && !assume_unsorted;

  int ret;

  parts.clear();

  if (sorted_omap) {
    string p;
    p = "part.";
    char buf[32];

    snprintf(buf, sizeof(buf), "%08d", marker);
    p.append(buf);

    ret = 0;// store->omap_get_vals(obj, header, p, num_parts + 1, parts_map);
  } else {
    ret = 0;//store->omap_get_all(obj, header, parts_map);
  }
  if (ret < 0)
    return ret;

  int i;
  int last_num = 0;

  uint32_t expected_next = marker + 1;

  for (i = 0, iter = parts_map.begin(); (i < num_parts || !sorted_omap) && iter != parts_map.end(); ++iter, ++i) {
    bufferlist& bl = iter->second;
    bufferlist::iterator bli = bl.begin();
    RGWUploadPartInfo info;
    try {
      ::decode(info, bli);
    } catch (buffer::error& err) {
      ldout(s->cct, 0) << "ERROR: could not part info, caught buffer::error" << dendl;
      return -EIO;
    }
    if (sorted_omap) {
      if (info.num != expected_next) {
        /* ouch, we expected a specific part num here, but we got a different one. Either
         * a part is missing, or it could be a case of mixed rgw versions working on the same
         * upload, where one gateway doesn't support correctly sorted omap keys for multipart
         * upload just assume data is unsorted.
         */
        return list_multipart_parts(store, s, upload_id, meta_oid, num_parts, marker, parts, next_marker, truncated, true);
      }
      expected_next++;
    }
    if (sorted_omap ||
      (int)info.num > marker) {
      parts[info.num] = info;
      last_num = info.num;
    }
  }

  if (sorted_omap) {
    if (truncated)
      *truncated = (iter != parts_map.end());
  } else {
    /* rebuild a map with only num_parts entries */

    map<uint32_t, RGWUploadPartInfo> new_parts;
    map<uint32_t, RGWUploadPartInfo>::iterator piter;

    for (i = 0, piter = parts.begin(); i < num_parts && piter != parts.end(); ++i, ++piter) {
      new_parts[piter->first] = piter->second;
      last_num = piter->first;
    }

    if (truncated)
      *truncated = (piter != parts.end());

    parts.swap(new_parts);
  }

  if (next_marker) {
    *next_marker = last_num;
  }

  return 0;
}

void RGWCompleteMultipart::pre_exec()
{
  rgw_bucket_object_pre_exec(s);
}
static int get_obj_attrs(RGWRados *store, struct req_state *s, rgw_obj& obj, map<string, bufferlist>& attrs)
{
  //RGWRados::Object op_target(store, s->bucket_info, *static_cast<RGWObjectCtx *>(s->obj_ctx), obj);
  //RGWRados::Object::Read read_op(&op_target);

  //read_op.params.attrs = &attrs;
  //read_op.params.perr = &s->err;

  return 0;//read_op.prepare(NULL, NULL);
}
void RGWCompleteMultipart::execute()
{
  RGWMultiCompleteUpload *parts;
  map<int, string>::iterator iter;
  RGWMultiXMLParser parser;
  string meta_oid;
  map<uint32_t, RGWUploadPartInfo> obj_parts;
  map<uint32_t, RGWUploadPartInfo>::iterator obj_iter;
  map<string, bufferlist> attrs;
  off_t ofs = 0;
  MD5 hash;
  char final_etag[CEPH_CRYPTO_MD5_DIGESTSIZE];
  char final_etag_str[CEPH_CRYPTO_MD5_DIGESTSIZE * 2 + 16];
  bufferlist etag_bl;
  rgw_obj meta_obj;
  rgw_obj target_obj;
  RGWMPObj mp;
//  RGWObjManifest manifest;
  uint64_t olh_epoch = 0;
  string version_id;

  ret = get_params();
  if (ret < 0)
    return;

  ret = 0;//get_system_versioning_params(s, &olh_epoch, &version_id);
  if (ret < 0) {
    return;
  }

  if (!data || !len) {
    ret = -ERR_MALFORMED_XML;
    return;
  }

  if (!parser.init()) {
    ret = -EIO;
    return;
  }

  if (!parser.parse(data, len, 1)) {
    ret = -ERR_MALFORMED_XML;
    return;
  }

  parts = static_cast<RGWMultiCompleteUpload *>(parser.find_first("CompleteMultipartUpload"));
  if (!parts || parts->parts.empty()) {
    ret = -ERR_MALFORMED_XML;
    return;
  }

  if ((int)parts->parts.size() > G.rgw_multipart_part_upload_limit) {
    ret = -ERANGE;
    return;
  }
#if 1
  mp.init(s->object.name, upload_id);
  meta_oid = mp.get_meta();

  int total_parts = 0;
  int handled_parts = 0;
  int max_parts = 1000;
  int marker = 0;
  bool truncated;

  uint64_t min_part_size = G.rgw_multipart_min_part_size;

  list<rgw_obj_key> remove_objs; /* objects to be removed from index listing */

  bool versioned_object = s->bucket_info.versioning_enabled();

  iter = parts->parts.begin();

  for (; iter!= parts->parts.end(); iter++)
  {
      char petag[CEPH_CRYPTO_MD5_DIGESTSIZE];
      hex_to_buf(iter->second.c_str(), petag, CEPH_CRYPTO_MD5_DIGESTSIZE);
      hash.Update((const byte *)petag, sizeof(petag));      
  }
  //meta_obj.init_ns(s->bucket, meta_oid, mp_ns);
  //meta_obj.set_in_extra_data(true);
  //meta_obj.index_hash_source = s->object.name;

  ret = get_obj_attrs(store, s, meta_obj, attrs);
  if (ret < 0) {
    ldout(s->cct, 0) << "ERROR: failed to get obj attrs, obj=" << meta_obj << " ret=" << ret << dendl;
    return;
  }
#if 0
  do {
    ret = list_multipart_parts(store, s, upload_id, meta_oid, max_parts, marker, obj_parts, &marker, &truncated);
    if (ret == -ENOENT) {
      ret = -ERR_NO_SUCH_UPLOAD;
    }
    if (ret < 0)
      return;

    total_parts += obj_parts.size();
    if (!truncated && total_parts != (int)parts->parts.size()) {
      ret = -ERR_INVALID_PART;
      return;
    }

    for (obj_iter = obj_parts.begin(); iter != parts->parts.end() && obj_iter != obj_parts.end(); ++iter, ++obj_iter, ++handled_parts) {
      uint64_t part_size = obj_iter->second.size;
      if (handled_parts < (int)parts->parts.size() - 1 &&
          part_size < min_part_size) {
        ret = -ERR_TOO_SMALL;
        return;
      }

      char petag[CEPH_CRYPTO_MD5_DIGESTSIZE];
      if (iter->first != (int)obj_iter->first) {
        ldout(s->cct, 0) << "NOTICE: parts num mismatch: next requested: " << iter->first << " next uploaded: " << obj_iter->first << dendl;
        ret = -ERR_INVALID_PART;
        return;
      }
      string part_etag = rgw_string_unquote(iter->second);
      if (part_etag.compare(obj_iter->second.etag) != 0) {
        ldout(s->cct, 0) << "NOTICE: etag mismatch: part: " << iter->first << " etag: " << iter->second << dendl;
        ret = -ERR_INVALID_PART;
        return;
      }

      hex_to_buf(obj_iter->second.etag.c_str(), petag, CEPH_CRYPTO_MD5_DIGESTSIZE);
      hash.Update((const byte *)petag, sizeof(petag));

      RGWUploadPartInfo& obj_part = obj_iter->second;

      /* update manifest for part */
      string oid = mp.get_part(obj_iter->second.num);
      rgw_obj src_obj;
      src_obj.init_ns(s->bucket, oid, mp_ns);

      //if (obj_part.manifest.empty()) {
      //  ldout(s->cct, 0) << "ERROR: empty manifest for object part: obj=" << src_obj << dendl;
      //  ret = -ERR_INVALID_PART;
      //  return;
      //} else {
      //  manifest.append(obj_part.manifest);
      //}

      rgw_obj_key remove_key;
      src_obj.get_index_key(&remove_key);

      remove_objs.push_back(remove_key);

      ofs += obj_part.size;
    }
  } while (truncated);
#endif  
  hash.Final((byte *)final_etag);

  buf_to_hex((unsigned char *)final_etag, sizeof(final_etag), final_etag_str);
  snprintf(&final_etag_str[CEPH_CRYPTO_MD5_DIGESTSIZE * 2],  sizeof(final_etag_str) - CEPH_CRYPTO_MD5_DIGESTSIZE * 2,
           "-%lld", (long long)parts->parts.size());
  etag = final_etag_str;
  ldout(s->cct, 10) << "calculated etag: " << final_etag_str << dendl;

  etag_bl.append(final_etag_str, strlen(final_etag_str) + 1);

  attrs[RGW_ATTR_ETAG] = etag_bl;

  target_obj.init(s->bucket, s->object.name);
  if (versioned_object) {
    ;//store->gen_rand_obj_instance_name(&target_obj);
  }

  RGWObjectCtx& obj_ctx = *static_cast<RGWObjectCtx *>(s->obj_ctx);

  obj_ctx.set_atomic(target_obj);

  RGWRados::Object op_target(store, s->bucket_info, *static_cast<RGWObjectCtx *>(s->obj_ctx), target_obj);
#if 0
  RGWRados::Object::Write obj_op(&op_target);

  obj_op.meta.manifest = &manifest;
  obj_op.meta.remove_objs = &remove_objs;

  obj_op.meta.ptag = &s->req_id; /* use req_id as operation tag */
  obj_op.meta.owner = s->owner.get_id();
  obj_op.meta.flags = PUT_OBJ_CREATE;

  ret = obj_op.write_meta(ofs, attrs);
  if (ret < 0)
    return;
#endif
  // remove the upload obj
  int r = 0;//store->delete_obj(*static_cast<RGWObjectCtx *>(s->obj_ctx), s->bucket_info, meta_obj, 0);
  if (r < 0) {
    ldout(store->ctx(), 0) << "WARNING: failed to remove object " << meta_obj << dendl;
  }
#endif
}

void RGWStatBucket::pre_exec()
{
  rgw_bucket_object_pre_exec(s);
}
void RGWStatBucket::execute()
{
  RGWUserBuckets buckets;
  bucket.bucket = s->bucket;
  buckets.add(bucket);
  map<string, RGWBucketEnt>& m = buckets.get_buckets();
  ret = 0;//store->update_containers_stats(m);
  if (!ret)
    ret = -EEXIST;
  if (ret > 0) {
    ret = 0;
    map<string, RGWBucketEnt>::iterator iter = m.find(bucket.bucket.name);
    if (iter != m.end()) {
      bucket = iter->second;
    } else {
      ret = -EINVAL;
    }
  }
}
bool verify_object_permission(struct req_state *s, int perm);

int RGWGetACLs::verify_permission()
{
  bool perm;
  if (!s->object.empty()) {
    perm = verify_object_permission(s, RGW_PERM_READ_ACP);
  } else {
    perm = verify_bucket_permission(s, RGW_PERM_READ_ACP);
  }
  if (!perm)
    return -EACCES;

  return 0;
}
void RGWGetACLs::pre_exec()
{
  rgw_bucket_object_pre_exec(s);
}
void RGWGetACLs::execute()
{
  stringstream ss;
#if 0
  ss<<"<AccessControlPolicy xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">"
     "<Owner> <ID>"<<"bwcpn"<<"</ID></Owner>";
  ss<<"<AccessControlList>"
            "<Grant>"
                "<Grantee xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" xsi:type=\"Canonical User\">"
                    "<ID>"<<"bwcpn"<<"</ID><DisplayName>"<<"bwcpn houu"<<"</DisplayName>"
                "</Grantee>"
                "<Permission>FULL_CONTROL</Permission>"
            "</Grant>"
          "</AccessControlList>";
  ss<<"</AccessControlPolicy>";                  
#else
  RGWAccessControlPolicy *acl = (!s->object.empty() ? s->object_acl : s->bucket_acl);
  RGWAccessControlPolicy_S3 *s3policy = static_cast<RGWAccessControlPolicy_S3 *>(acl);
  s3policy->to_xml(ss);
#endif
  acls = ss.str();
}
bool RGWCopyObj::parse_copy_location(const string& url_src, string& bucket_name, rgw_obj_key& key)
{
  string name_str;
  string params_str;

  int pos = url_src.find('?');
  if (pos < 0) {
    name_str = url_src;
  } else {
    name_str = url_src.substr(0, pos);
    params_str = url_src.substr(pos + 1);
  }


  string dec_src;

  url_decode(name_str, dec_src);
  const char *src = dec_src.c_str();

  if (*src == '/') ++src;

  string str(src);

  pos = str.find("/");
  if (pos <= 0)
    return false;

  bucket_name = str.substr(0, pos);
  key.name = str.substr(pos + 1);

  if (key.name.empty()) {
    return false;
  }

  if (!params_str.empty()) {
    RGWHTTPArgs args;
    args.set(params_str);
    args.parse();

    key.instance = args.get("versionId", NULL);
  }

  return true;
}
static void format_xattr(std::string &xattr)
{
  /* If the extended attribute is not valid UTF-8, we encode it using quoted-printable
   * encoding.
   */
  if ((check_utf8(xattr.c_str(), xattr.length()) != 0) ||
      (check_for_control_characters(xattr.c_str(), xattr.length()) != 0)) {
    static const char MIME_PREFIX_STR[] = "=?UTF-8?Q?";
    static const int MIME_PREFIX_LEN = sizeof(MIME_PREFIX_STR) - 1;
    static const char MIME_SUFFIX_STR[] = "?=";
    static const int MIME_SUFFIX_LEN = sizeof(MIME_SUFFIX_STR) - 1;
    int mlen = mime_encode_as_qp(xattr.c_str(), NULL, 0);
    char *mime = new char[MIME_PREFIX_LEN + mlen + MIME_SUFFIX_LEN + 1];
    strcpy(mime, MIME_PREFIX_STR);
    mime_encode_as_qp(xattr.c_str(), mime + MIME_PREFIX_LEN, mlen);
    strcpy(mime + MIME_PREFIX_LEN + (mlen - 1), MIME_SUFFIX_STR);
    xattr.assign(mime);
    delete [] mime;
  }
}

/**
 * Get the HTTP request metadata out of the req_state as a
 * map(<attr_name, attr_contents>, where attr_name is RGW_ATTR_PREFIX.HTTP_NAME)
 * s: The request state
 * attrs: will be filled up with attrs mapped as <attr_name, attr_contents>
 *
 */
static void rgw_get_request_metadata(CephContext *cct,
                                     struct req_info& info,
                                     map<string, bufferlist>& attrs,
                                     const bool allow_empty_attrs = true)
{
  map<string, string>::iterator iter;
  for (iter = info.x_meta_map.begin(); iter != info.x_meta_map.end(); ++iter) {
    const string &name(iter->first);
    string &xattr(iter->second);

    if (allow_empty_attrs || !xattr.empty()) {
      ldout(cct, 10) << "x>> " << name << ":" << xattr << dendl;
      format_xattr(xattr);
      string attr_name(RGW_ATTR_PREFIX);
      attr_name.append(name);
      map<string, bufferlist>::value_type v(attr_name, bufferlist());
      std::pair < map<string, bufferlist>::iterator, bool > rval(attrs.insert(v));
      bufferlist& bl(rval.first->second);
      bl.append(xattr.c_str(), xattr.size() + 1);
    }
  }
}

int RGWCopyObj::init_common()
{
  if (if_mod) {
    if (parse_time(if_mod, &mod_time) < 0) {
      ret = -EINVAL;
      return ret;
    }
    mod_ptr = &mod_time;
  }

  if (if_unmod) {
    if (parse_time(if_unmod, &unmod_time) < 0) {
      ret = -EINVAL;
      return ret;
    }
    unmod_ptr = &unmod_time;
  }

  bufferlist aclbl;
  dest_policy.encode(aclbl);

  attrs[RGW_ATTR_ACL] = aclbl;
  rgw_get_request_metadata(s->cct, s->info, attrs);

  map<string, string>::iterator iter;
  for (iter = s->generic_attrs.begin(); iter != s->generic_attrs.end(); ++iter) {
    bufferlist& attrbl = attrs[iter->first];
    const string& val = iter->second;
    attrbl.append(val.c_str(), val.size() + 1);
  }

  return 0;
}
int RGWCopyObj::verify_permission()
{
  ret = get_params();
  if (ret < 0)
    return ret; 
  return 0;
}
void RGWCopyObj::pre_exec()
{
  rgw_bucket_object_pre_exec(s);
}
void RGWCopyObj::execute()
{
  uid_t old_uid = 0;
  int id;
  
  string src_path = G.buckets_root + string("/") + src_bucket_name + string("/") + src_object.name;
  string dst_path = G.buckets_root + string("/") + dest_bucket_name + string("/") + dest_object;

  if (init_common() < 0)
    return;
  old_uid = drop_privs(s->user.auid);
  
  if (::rename(src_path.c_str(), dst_path.c_str()) < 0)
  {
    ret = -errno;
  }
  else
  {
    ret = 0;
  }
  id = restore_privs(old_uid);
  
#if 0
  rgw_obj src_obj(src_bucket, src_object);
  rgw_obj dst_obj(dest_bucket, dest_object);

  RGWObjectCtx& obj_ctx = *static_cast<RGWObjectCtx *>(s->obj_ctx);
  obj_ctx.set_atomic(src_obj);
  obj_ctx.set_atomic(dst_obj);

  encode_delete_at_attr(delete_at, attrs);

  ret = store->copy_obj(obj_ctx,
                        s->user.user_id,
                        client_id,
                        op_id,
                        &s->info,
                        source_zone,
                        dst_obj,
                        src_obj,
                        dest_bucket_info,
                        src_bucket_info,
                        &src_mtime,
                        &mtime,
                        mod_ptr,
                        unmod_ptr,
                        if_match,
                        if_nomatch,
                        attrs_mod,
                        attrs, RGW_OBJ_CATEGORY_MAIN,
                        olh_epoch,
			delete_at,
                        (version_id.empty() ? NULL : &version_id),
                        &s->req_id, /* use req_id as tag */
                        &etag,
                        &s->err,
                        copy_obj_progress_cb, (void *)this
                        );
#endif
}
void RGWCopyObj::progress_cb(off_t ofs)
{
  if (1/*!s->cct->_conf->rgw_copy_obj_progress*/)
    return;

  if (1/*ofs - last_ofs < s->cct->_conf->rgw_copy_obj_progress_every_bytes*/)
    return;

  send_partial_response(ofs);

  last_ofs = ofs;
}
int RGWGetBucketLocation::verify_permission()
{
  //if (s->user.user_id.compare(s->bucket_owner.get_id()) != 0)
  //  return -EACCES;

  return 0;
}
