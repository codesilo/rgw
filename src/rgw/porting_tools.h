/*
 * =====================================================================================
 *       Copyright (c), 2013-2020, Sobey.
 *       Filename:  porting_tools.h
 *
 *    Description:  
 *         Others:
 *
 *        Version:  1.0
 *        Created:  2015/12/25 15:30:05
 *       Revision:  none
 *       Compiler:  gcc
 *
 *         Author:  Sean. Hou (hwt), houwentaoff@gmail.com
 *   Organization:  Sobey
 *
 * =====================================================================================
 */

#ifndef __PORTING_TOOLS_H__
#define __PORTING_TOOLS_H__

#include <string>
#include "include/types.h"
#include "porting_common.h"

class RGWRados;
class RGWObjectCtx;
struct RGWObjVersionTracker;

struct obj_version;

struct sys_info
{
    typedef enum {
        USER = 1,
        BUCNET_STAT,  
    }category_e;
    typedef enum {
        PART = 1,
        FULL,
    }region_e;
    sys_info():display_name(""), user_email(""), max_buckets(0), bucket_max_size_kb(0),
    bucket_max_objects(0),bucket_enabled(false), user_max_size_kb(0), user_max_objects(0),
    user_enabled(false),file(""),total_size(0),total_size_rounded(0),num_entries(0){}
    static void get_params(void* obj, const char *name, const char *val);    
    static void set_params(void* obj, sys_info::category_e category, const char *name, const char *val);    
    void set_file(string path){file = path;}
    int sync_stat(sys_info::region_e op, const char *var_name, const char *val);
    int sync_user(sys_info::region_e op, const char *var_name, const char *val);
    string file;
    /* user info */
    string display_name, user_email;
    uint32_t max_buckets;
    
    int64_t bucket_max_size_kb;
    int64_t bucket_max_objects;
    bool    bucket_enabled;

    int64_t user_max_size_kb;
    int64_t user_max_objects;
    bool    user_enabled;
    /* bucket stat */
    uint64_t total_size;
    uint64_t total_size_rounded;
    uint64_t num_entries;    
};

int rgw_put_system_obj(RGWRados *rgwstore, rgw_bucket& bucket, string& oid, const char *data, size_t size, bool exclusive,
                       RGWObjVersionTracker *objv_tracker, time_t set_mtime, map<string, bufferlist> *pattrs = NULL);
int rgw_get_system_obj(RGWRados *rgwstore, RGWObjectCtx& obj_ctx, rgw_bucket& bucket, const string& key, bufferlist& bl,
                       RGWObjVersionTracker *objv_tracker, time_t *pmtime, map<string, bufferlist> *pattrs = NULL,
                       rgw_cache_entry_info *cache_info = NULL);

int rgw_tools_init(CephContext *cct);
void rgw_tools_cleanup();
const char *rgw_find_mime_by_ext(string& ext);

#endif
