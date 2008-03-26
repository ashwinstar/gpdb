#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <gpcc_file.h>

#ifdef WIN32
#include <io.h>
#else
#include <unistd.h>
#endif


APR_DECLARE(size_t) gpcc_file_read_fully(const char *fn, char **buf, apr_pool_t *ap)
{
  FILE *fp = 0;
  size_t len;

  *buf = 0;
  if ((fp = fopen(fn, "rb")) == 0)
    goto bail;
  
  if (fseek(fp, 0, SEEK_END) == -1)
    goto bail;

  if ((len = ftell(fp)) == -1)
    goto bail;

  if (fseek(fp, 0, SEEK_SET) == -1)
    goto bail;

  *buf = apr_palloc(ap, len+1);
  if (*buf == 0)
    goto bail;
  
  if (fread(*buf, 1, len, fp) != len)
    goto bail;

  if (fp) fclose(fp);
  
  (*buf)[len] = 0;
  
  return len;
  
bail:
  if (fp) fclose(fp);
  return 0;
}


APR_DECLARE(int) gpcc_file_exists(const char *fpath)
{
    int fd;
    if (-1 == (fd = open(fpath, O_RDONLY, 0600))) {
	gpcc_warning(FLINE, "unable to open file %s", fpath);
	return 0;
    }

    close(fd);
    return 1;
}


APR_DECLARE(int) gpcc_file_directory_exists(const char *dirname) 
{
    DIR *dp = 0;

    if ((dp = opendir(dirname)) == 0) {
	return 0;
    }

    closedir(dp);
    return 1;
}

APR_DECLARE(int) gpcc_file_directory_writable(const char *dirname)
{
    FILE *fp = 0;
    char path[1024];

    if (! gpcc_file_directory_exists(dirname)) {
	gpcc_warning(FLINE, "directory %s not exists", dirname);
	return 0;
    }

    sprintf(path, "%s/.writeable_test", dirname);
    if ((fp = fopen(path, "wb")) == 0){
	gpcc_warning(FLINE, "data directory is not writeable");
	return 0;
    }

    if (fp) fclose(fp);
    remove(path);

    return 1;
}
