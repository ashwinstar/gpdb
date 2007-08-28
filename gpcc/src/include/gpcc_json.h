#ifndef GPCC_JSON_H
#define GPCC_JSON_H

#include <json/json.h>

/**
 *  get string from json_object
 *  @param object json object
 *  @param key  key value
 *  @param defvalue default value
 *  @return string value if exists otherwise returns default value
 */
char *gpcc_json_object_get_string(struct json_object *object, char *key, char *defvalue);

/**
 *  get int from json_object
 *  @param object json object
 *  @param key  key value
 *  @param defvalue default value
 *  @return integer value if exists otherwise returns default value
 */
int gpcc_json_object_get_int(struct json_object *object, char *key, int defvalue);


/**
 *  create hostlist object
 *
 *  object contains "master" array and "segment" array
 *
 *  @param masters  list of master host and terminate with NULL pointer
 *  @param segments list of segment host and terminate with NULL pointer
 *  @return json_object
 */
struct json_object *gpcc_json_hostlist(const char * const *masters, const char *const *segments);


/**
 *  create segment   request object
 *
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
 *   gpdb.max_connect   =       25
 *   gpdb.shared_buffers =      128Mb
 *   gpdb.user          =       username
 *   gpdb.password      =       password
 *   node.X             =       (x.x.x.a, x.x.x.b, x.x.x.c, x.x.x.d) list of network addresses
 *   gpdb.seg.m.X       =       node.X:/db/seg.m.X:0   #node, data_dir, cpu affinity
 *   
 *  @param segment   segment name e.g. "seg.X.X"
 *  @param action    start/stop/set_mode
 *  @param mode      ro/rw
 *  @param host      host name
 *  @param port      port number
 *  @param data_dir  data directory
 *  @return json_object
 */
struct json_object *gpcc_json_segment_request(const char *segment, const char *action, const char *mode, 
					      const char *host, int port, const char *data_dir);


/** 
 *  create segment reply object
 *
 *  @param status    ok|failed
 *  @param reason    reason of failure
 *  @return json_object
 */
struct json_object *gpcc_json_segment_reply(const char *status, const char *reason);


/**
 *  create host request object
 *  @param host     host name
 *  @param action   start|stop
 *  @param mode     
 *  @return json_object
 */
struct json_object *gpcc_json_host_request(const char *host, const char *action, const char *mode);


/**
 *  create array request object
 *
 *  @param action   stop|stop|set_mode
 *  @param mode
 *  @return json_object
 */
struct json_object *gpcc_json_array_request(const char *action, const char *mode);


/**
 *  create alert object
 *
 *  @param segment   segment name
 *  @param alert     alert message
 *  @return json_object
 */
struct json_object *gpcc_json_alert(const char *segment, const char *alert);

/**
 *  create error object
 *  
 *  @param errmsg    error message
 *  @return json_object
 */
struct json_object *gpcc_json_error(const char *errmsg) ;


/**
 *  create status object
 *
 *  @param segment     segment name
 *  @param health      health status
 *  @param mode
 *  @param alert       alert message
 *  @return json_object
 */
struct json_object *gpcc_json_status(const char *segment, const char *health, const char *mode, const char *alert);

/**
 *  free json object
 * 
 *  @param obj   object to free
 *  @return json_object
 */
void gpcc_json_object_free(struct json_object *obj);



#endif
