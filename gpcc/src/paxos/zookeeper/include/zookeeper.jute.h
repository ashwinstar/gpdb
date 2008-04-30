#ifndef __ZOOKEEPER_JUTE__
#define __ZOOKEEPER_JUTE__

struct Id {
    char * scheme;
    char * id;
};
struct ACL {
    int32_t perms;
    struct Id id;
};
struct Stat {
    int64_t czxid;
    int64_t mzxid;
    int64_t ctime;
    int64_t mtime;
    int32_t version;
    int32_t cversion;
    int32_t aversion;
    int64_t ephemeralOwner;
};
struct ACL_vector {
    int32_t count;
    struct ACL *data;
};
struct String_vector {
    int32_t count;
    char * *data;
};

#endif //ZOOKEEPER_JUTE__
