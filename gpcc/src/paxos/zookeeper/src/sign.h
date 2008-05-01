/* sign.h */

#ifndef SIGN_H
#define SIGN_H

#define SIGNSTRING		"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx "
#define SIGNSTRING_LENGTH	33

int chubby_readPaxosKey(zhandle_t*thiz);
void chubby_sign(zhandle_t*thiz,char *a,const char*b,int len,const struct sockaddr*f,const struct sockaddr*t);

#endif
