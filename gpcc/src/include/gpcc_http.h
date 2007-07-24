#ifndef GPCC_HTTP_H
#define GPCC_HTTP_H

#include <gpcc_net.h>

#define HTTP_NET_PROTO          "http/1.0"

typedef struct gpcc_http_request_t gpcc_http_request_t;

struct gpcc_http_request_t {
    gpcc_net_request_t *net;
    int pc;
    char *pname[50];
    char *pvalue[50];
    char *cpath;  
};

APR_DECLARE(gpcc_http_request_t *) gpcc_http_parse_request(gpcc_net_request_t *req, apr_pool_t *p);

/**
 *  Returns value of HTTP request parameter name/value pair by name
 */
APR_DECLARE(const char *) gpcc_http_request_get_param(gpcc_http_request_t *req, const char *name, const char *def);

/**
 *  Returns integer value of HTTP request parameter name/value pair by name
 */
APR_DECLARE(int) gpcc_http_request_get_param_int(gpcc_http_request_t *req, const char *name, int def);

/**
 *  Returns float value of HTTP request parameter name/value pair by name
 */
APR_DECLARE(float) gpcc_http_request_get_param_float(gpcc_http_request_t *req, const char *name, float def);

/**
 *  Returns method of HTTP request, for example, GET, POST, or PUT.
 */
APR_DECLARE(const char *) gpcc_http_request_get_method(gpcc_http_request_t *req);

/**
 *  Returns data of HTTP request
 */
APR_DECLARE(const char *) gpcc_http_request_get_data(gpcc_http_request_t *req, int *len);

/**
 *  Returns the portion of the request URI that indicates the context of the request
 */
APR_DECLARE(const char *) gpcc_http_request_get_context_path(gpcc_http_request_t *req);


#endif
