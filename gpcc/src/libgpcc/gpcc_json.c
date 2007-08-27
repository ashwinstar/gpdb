#include <gpcc_json.h>

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

struct json_object *gpcc_json_segment_request(const char *segment, const char *action, const char *mode, 
					      const char *host, int port, const char *data_dir)
{
    struct json_object *obj = 0;

    obj = json_object_new_object();
    json_object_object_add(obj, "seg", json_object_new_string((char *)segment));
    json_object_object_add(obj, "action", json_object_new_string((char *)action));
    json_object_object_add(obj, "mode", json_object_new_string((char *)mode));
    json_object_object_add(obj, "host", json_object_new_string((char *)host));
    json_object_object_add(obj, "port", json_object_new_int(port));
    json_object_object_add(obj, "dir", json_object_new_string((char *)data_dir));

    return obj;
}


struct json_object *gpcc_json_segment_reply(const char *status, const char *reason)
{
    const char *r = (reason ? reason : "");
    struct json_object *obj = 0;

    obj = json_object_new_object();
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

struct json_object *gpcc_json_status(const char *segment, const char *health, const char *mode, const char *alert)
{
    struct json_object *obj = 0;
    
    obj = json_object_new_object();
    json_object_object_add(obj, "seg", json_object_new_string((char *)segment));
    json_object_object_add(obj, "health", json_object_new_string((char *)health));
    json_object_object_add(obj, "mode", json_object_new_string((char *)mode));
    json_object_object_add(obj, "alert", json_object_new_string((char *)alert));

    return obj;
}

void gpcc_json_object_free(struct json_object *obj)
{
    json_object_put(obj);
    
}
