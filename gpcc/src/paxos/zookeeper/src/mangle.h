/* mangle.h */

#ifndef MANGLE_H
#define MANGLE_H

int mangle_path(char*realpath,int max_realpath_len,const char*path);
int unmangle_path(char*path);
char mangle_setChar(char*path,char what);

#endif
