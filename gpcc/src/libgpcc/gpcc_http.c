#include <stdlib.h>
#include <ctype.h>
#include <apr_strings.h>
#include <gpcc_http.h>

#define ISXDIGIT(x) (isxdigit((int) ((unsigned char)x)))

char *url_unescape(const char *string, int length, int *olen, apr_pool_t *p)
{
  int alloc = (length?length:(int)strlen(string))+1;
  char *ns = apr_palloc(p, alloc);
  unsigned char in;
  int strindex=0;
  long hex;

  if( !ns )
    return NULL;

  while(--alloc > 0) {
    in = *string;
    if(('%' == in) && ISXDIGIT(string[1]) && ISXDIGIT(string[2])) {
      /* this is two hexadecimal digits following a '%' */
      char hexstr[3];
      char *ptr;
      hexstr[0] = string[1];
      hexstr[1] = string[2];
      hexstr[2] = 0;

      hex = strtol(hexstr, &ptr, 16);

      in = (unsigned char)hex; /* this long is never bigger than 255 anyway */

      string+=2;
      alloc-=2;
    }

    ns[strindex++] = in;
    string++;
  }
  ns[strindex]=0; /* terminate it */

  if(olen)
    /* store output size */
    *olen = strindex;
  return ns;
}

APR_DECLARE(apr_status_t) gpcc_http_parse_request(gpcc_net_request_t *req, apr_pool_t *pool)
{
    gpcc_http_request_t *http = (gpcc_http_request_t *) req;
    char *s, *last, *tok, *p;
    int i;

    if (req->argc != 3 || apr_strnatcasecmp(req->argv[2], HTTP_NET_PROTO) != 0) {
	gpcc_log_error("invalid HTTP request");
	return APR_EINVAL;
    }
    
    /* parse URL */
    http->cpath = req->argv[1];
    if ((s = strchr(req->argv[1], '?')) != 0) {
	*s = 0;
	s++;
	for (i = 0 , tok = apr_strtok(s, "&", &last) ; tok ; i++, tok = apr_strtok(0, "&", &last)) {
	    if ((p = strchr(tok, '=')) == 0) {
		http->pname[i] = apr_pstrdup(pool, tok);
		http->pvalue[i] = "";
	    }
	    else {
		http->pname[i] = apr_pstrndup(pool, tok, p - tok);
		http->pvalue[i] = url_unescape(p+1, 0, 0, pool);
	    }
	}
	http->pc = i;
    }
    
    return 0;
}


static const char *_http_request_get_param(gpcc_http_request_t *req, const char *name) 
{
    int i;
    for (i = 0 ; i < req->pc ; i++) {
	if (apr_strnatcmp(req->pname[i], name) == 0)
	    return req->pvalue[i];
    }
    return 0;
}
/**
 *  Returns value of HTTP request parameter name/value pair by name
 */
APR_DECLARE(const char *) gpcc_http_request_get_param(gpcc_http_request_t *req, const char *name, const char *def)
{
    const char *v = _http_request_get_param(req, name);
    return (v ? v : def);
}

/**
 *  Returns integer value of HTTP request parameter name/value pair by name
 */
APR_DECLARE(int) gpcc_http_request_get_param_int(gpcc_http_request_t *req, const char *name, int def)
{
    const char *v = _http_request_get_param(req, name);
    return (v ? atoi(v) : def);
}

/**
 *  Returns float value of HTTP request parameter name/value pair by name
 */
APR_DECLARE(float) gpcc_http_request_get_param_float(gpcc_http_request_t *req, const char *name, float def)
{
    const char *v = _http_request_get_param(req, name);
    return (v ? atof(v) : def);

}

/**
 *  Returns method of HTTP request, for example, GET, POST, or PUT.
 */
APR_DECLARE(const char *) gpcc_http_request_get_method(gpcc_http_request_t *req)
{
    return req->net.argv[0];
}

/**
 *  Returns data of HTTP request
 */
APR_DECLARE(const char *) gpcc_http_request_get_data(gpcc_http_request_t *req, int *len)
{
    *len = req->net.datalen;
    return req->net.data;
}

/**
 *  Returns the portion of the request URI that indicates the context of the request
 */
APR_DECLARE(const char *) gpcc_http_request_get_context_path(gpcc_http_request_t *req)
{
    return req->cpath;
}

