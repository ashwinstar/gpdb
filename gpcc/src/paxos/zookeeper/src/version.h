/* version.h */

#ifndef VERSION_H
#define VERSION_H

int getVersion(struct zframe*z,char*path,int*version);
int setVersion(struct zframe*z,char*path,int version);
int deleteVersion(struct zframe*z,char*path);

#endif
