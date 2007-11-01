#include <gpcc_json.h>

struct json_object *gpcc_json_object_get_object(struct json_object *object, char *key, struct json_object *def)
{
    struct json_object *keyobj;
    keyobj = json_object_object_get(object, key);
    if (!keyobj) {
	return def;
    }

    return keyobj;
}

char *gpcc_json_object_get_string(struct json_object *object, char *key, char *defvalue)
{
    struct json_object *keyobj;

    keyobj = json_object_object_get(object, key);
    if (!keyobj){
	return defvalue;
    }

    return json_object_get_string(keyobj);
}

int gpcc_json_object_get_int(struct json_object *object, char *key, int defvalue)
{
    struct json_object *keyobj;

    keyobj = json_object_object_get(object, key);
    if (!keyobj){
	return defvalue;
    }

    return json_object_get_int(keyobj);
}



struct json_object *gpcc_json_hostlist(const char * const *masters, const char *const *segments) {
    struct json_object *obj = 0;
    struct json_object *master_array = 0;
    struct json_object *segment_array = 0;
    int i;

    obj = json_object_new_object();
    master_array = json_object_new_array();

    for (i = 0 ; masters[i] ; i++) {
	json_object_array_add(master_array, json_object_new_string((char *)masters[i]));
    }

    json_object_object_add(obj, "master", master_array);

    segment_array = json_object_new_array();
    
    for (i = 0 ; segments[i] ; i++) {
	json_object_array_add(segment_array, json_object_new_string((char *) segments[i]));
    }

    json_object_object_add(obj, "segment", segment_array);

    return obj;
}


/**
 *   seg              =         gpdb.seg.m.0 or gpdb.seg.X.X   X is an integer
 *   action           =         init/start/stop/status
 *   mode             =         ro/rw     smart/immediate/fast
 *   host             =         hostname  (node.X or (x.x.x.a, x.x.x.b, x.x.x.c, x.x.x.d))
 *   dir              =         data directory
 *   port             =         database port number
 *
 *   additional parameters for initdb
 *   dbid             =         dbid
 *   content          =         content
 *
 *   gpdb.name        =         mygpdb
 *   gpdb.encoding    =         UNICODE
 *   gpdb.locale      =         UNICODE
 *   gpdb.trusted_shell =       ssh
 *   gpdb.checkpoint_segments =       check point segment default = 31
 *   gpdb.user          =       username
 *   gpdb.password      =       password
 *   node.X             =       (x.x.x.a, x.x.x.b, x.x.x.c, x.x.x.d) list of network addresses
 *   gpdb.seg.m.X       =       node.X:/db/seg.m.X:0   #node, data_dir, cpu affinity
 *   
 */

struct json_object *gpcc_json_segment_request(struct json_object *_obj, 
					      const char *segment, const char *action, const char *mode, 
					      const char *host, int port, const char *data_dir)
{
    struct json_object *obj = _obj;
    if (!obj)
	obj = json_object_new_object();
    json_object_object_add(obj, "seg", json_object_new_string((char *)segment));
    json_object_object_add(obj, "action", json_object_new_string((char *)action));
    json_object_object_add(obj, "mode", json_object_new_string((char *)mode));
    json_object_object_add(obj, "host", json_object_new_string((char *)host));
    json_object_object_add(obj, "port", json_object_new_int(port));
    json_object_object_add(obj, "dir", json_object_new_string((char *)data_dir));

    return obj;
}


struct json_object *gpcc_json_segment_reply(const char *segment, const char *status, const char *reason)
{
    const char *r = (reason ? reason : "");
    struct json_object *obj = 0;

    obj = json_object_new_object();
    json_object_object_add(obj, "seg", json_object_new_string((char *) segment));
    json_object_object_add(obj, "status", json_object_new_string((char *)status));
    json_object_object_add(obj, "reason", json_object_new_string((char *)r));

    return obj;
}

struct json_object *gpcc_json_host_request(const char *host, const char *action, const char *mode)
{
    struct json_object *obj = 0;
    
    obj = json_object_new_object();
    json_object_object_add(obj, "host", json_object_new_string((char *)host));
    json_object_object_add(obj, "action", json_object_new_string((char *)action));
    json_object_object_add(obj, "mode", json_object_new_string((char *)mode));
    
    return obj;
}

struct json_object *gpcc_json_array_request(const char *action, const char *mode)
{
    struct json_object *obj = 0;

    obj = json_object_new_object();
    json_object_object_add(obj, "action", json_object_new_string((char *)action));
    json_object_object_add(obj, "mode", json_object_new_string((char *)mode));

    return obj;
}

struct json_object *gpcc_json_array_reply(const char *action, const char *status, const char *reason)
{
   const char *r = (reason ? reason : "");
    struct json_object *obj = 0;

    obj = json_object_new_object();
    json_object_object_add(obj, "action", json_object_new_string((char *) action));
    json_object_object_add(obj, "status", json_object_new_string((char *)status));
    json_object_object_add(obj, "reason", json_object_new_string((char *)r));

    return obj;
}

struct json_object *gpcc_json_http_reply(const char *action, const char *status, const char *errmsg)
{
    const char *r = (errmsg ? errmsg : "");
    struct json_object *obj = 0;
    struct json_object *errobj = 0;

    obj = json_object_new_object();
    json_object_object_add(obj, "action", json_object_new_string((char *) action));
    json_object_object_add(obj, "status", json_object_new_string((char *)status));
    errobj = json_object_new_object();
    json_object_object_add(errobj, "_", json_object_new_string((char *) r));
    json_object_object_add(obj, "errmsg", errobj);

    return obj;
}

struct json_object *gpcc_json_http_status_reply(const char *array, const char *status, const char *result) 
{
    struct json_object *obj = 0;
    struct json_object *resobj = 0;
    struct json_object *arrobj = 0;

    obj = json_object_new_object();
    json_object_object_add(obj, "status", json_object_new_string((char *)status));

    if (array) {
	json_object_object_add(obj, "array", json_object_new_string((char *) array));
    }

    resobj = json_object_new_object();

    arrobj = json_object_new_array();
    json_object_array_add(arrobj, json_object_new_string((char *) result));
    json_object_object_add(resobj, "result", arrobj);
    json_object_object_add(resobj, "total", json_object_new_int(1));
    json_object_object_add(resobj, "offset", json_object_new_int(0));
    json_object_object_add(obj, "rset", resobj);
    
    return obj;
}


struct json_object *gpcc_json_alert(const char *segment, const char *alert) 
{
    struct json_object *obj = 0;
    
    obj = json_object_new_object();
    json_object_object_add(obj, "seg", json_object_new_string((char *)segment));
    json_object_object_add(obj, "alert", json_object_new_string((char *)alert));

    return obj;
}

struct json_object *gpcc_json_error(const char *msg) 
{
    struct json_object *obj = 0;
    
    obj = json_object_new_object();
    json_object_object_add(obj, "msg", json_object_new_string((char *)msg));
    
    return obj;
}

struct json_object *gpcc_json_status_request(const char *segment)
{
    struct json_object *obj = 0;
    
    obj = json_object_new_object();
    if (segment) {
	json_object_object_add(obj, "seg", json_object_new_string((char *) segment));
    }
    json_object_object_add(obj, "action", json_object_new_string("status"));

    return obj;
}

struct json_object *gpcc_json_status_reply(const char *segment, const char *health, const char *alert, const char *status)
{
    struct json_object *obj = 0;
    
    obj = json_object_new_object();
    json_object_object_add(obj, "seg", json_object_new_string((char *)segment));
    json_object_object_add(obj, "health", json_object_new_string((char *)health));
    json_object_object_add(obj, "status", json_object_new_string((char *)status));
    json_object_object_add(obj, "alert", json_object_new_string((char *)alert));

    return obj;
}

void gpcc_json_object_free(struct json_object *obj)
{
    json_object_put(obj);
    
}
