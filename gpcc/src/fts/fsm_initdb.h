
static struct json_object *config_to_json_object() 
{
    struct json_object *obj;
    char key[128];
    int i;
    const char *value;
    int intvalue;

    obj = json_object_new_object();

    strcpy(key, "gpdb.name");
    value = config_getx(gpfts.config, key, "mygpdb");
    json_object_object_add(obj, key, json_object_new_string((char *)value));    

    strcpy(key, "gpdb.user");
    value = config_getx(gpfts.config, key, getenv("USER"));
    json_object_object_add(obj, key, json_object_new_string((char *)value));    

    strcpy(key, "gpdb.password");
    value = config_getx(gpfts.config, key, "mpparray");
    json_object_object_add(obj, key, json_object_new_string((char *)value));    

    strcpy(key, "gpdb.max_connect");
    intvalue = config_get_or_set_int(gpfts.config, key, 25);
    json_object_object_add(obj, key, json_object_new_int(intvalue));

    strcpy(key, "gpdb.encoding");
    value = config_getx(gpfts.config, key, "UNICODE");
    json_object_object_add(obj, key, json_object_new_string((char *)value));    

    strcpy(key, "gpdb.locale");
    value = config_getx(gpfts.config, key, "UNICODE");
    json_object_object_add(obj, key, json_object_new_string((char *)value));    

    strcpy(key, "gpdb.shared_buffers");
    value = config_getx(gpfts.config, key, "128MB");
    json_object_object_add(obj, key, json_object_new_string((char *)value));    

    strcpy(key, "gpdb.checkpoint_segments");
    intvalue = config_get_or_set_int(gpfts.config, key, 31);
    json_object_object_add(obj, key, json_object_new_int(intvalue));

    strcpy(key, "gpdb.seg.master_port");
    intvalue = config_get_or_set_int(gpfts.config, key, DEFAULT_QD_PORT);
    json_object_object_add(obj, key, json_object_new_int(intvalue));

    strcpy(key, "gpdb.seg.base_port");
    intvalue = config_get_or_set_int(gpfts.config, key, DEFAULT_QE_BASEPORT);
    json_object_object_add(obj, key, json_object_new_int(intvalue));

    strcpy(key, "gpdb.seg.mirror_base_port");
    intvalue = config_get_or_set_int(gpfts.config, key, DEFAULT_MIRROR_QE_BASEPORT);
    json_object_object_add(obj, key, json_object_new_int(intvalue));

    
    for (i = 0 ;  ; i++) {
	sprintf(key, "node.%d", i);
	value = config_get(gpfts.config, key);
	if (!value) 
	    break;

	json_object_object_add(obj, key, json_object_new_string((char *)value));    
    }

    for (i = 0 ; ; i++) {
	sprintf(key, "gpdb.seg.m.%d", i);
	value = config_get(gpfts.config, key);
	if (!value)
	    break;
	json_object_object_add(obj, key, json_object_new_string((char *)value));    
    }

    for (i = 0 ; ; i++) {
	sprintf(key, "gpdb.seg.%d.0", i);
	value = config_get(gpfts.config, key);
	if (!value)
	    break;
	json_object_object_add(obj, key, json_object_new_string((char *)value));    

	sprintf(key, "gpdb.seg.%d.1", i);
	value = config_get(gpfts.config, key);
	if (!value)
	    continue;
	json_object_object_add(obj, key, json_object_new_string((char *)value));    
    }



    return obj;
}

static apr_status_t initdb_do(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    char seg[128];
    const char *value;
    struct json_object *obj;
    int i;

    /* prepare json object */
    obj = config_to_json_object();

    /* init segment */
    for (i = 0 ; ; i++) {
	sprintf(seg, "gpdb.seg.%d.0", i);

	value = config_get(gpfts.config, seg);
	if (!value) {
	    break;
	}

	sprintf(seg, "gpdb.seg.%d.1", i);    
	value = config_get(gpfts.config, seg);
	if (!value) {
	    continue;
	}
    }

    /* init QD */
    for (i = 0 ; ; i++) {
	sprintf(seg, "gpdb.seg.m.%d", i);
	value = config_get(gpfts.config, seg);
	if (!value) {
	    break;
	}


    }


    return 0;
}

static apr_status_t initdb_reply(gpcc_event_t *ev, void *_cmdslot)
{


    return 0;
}

static apr_status_t initdb_success(gpcc_event_t *ev, void *_cmdslot)
{


    return 0;
}

static apr_status_t initdb_failure(gpcc_event_t *ev, void *_cmdslot)
{


    return 0;
}
