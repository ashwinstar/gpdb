/*-------------------------------------------------------------------------
 *
 * execworkfile.c
 *    Management of temporary work files used by the executor nodes.
 *
 * Copyright (c) 2010. Greenplum Inc.
 *
 * WorkFiles provide a general interface to different implementations of
 * temporary files used by the executor nodes.
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "storage/buffile.h"
#include "storage/bfz.h"
#include "executor/execWorkfile.h"


static void ExecWorkfile_SetFlags(ExecWorkFile *workfile);
static void ExecWorkfile_AdjustBFZSize(ExecWorkFile *workfile, int64 file_size);

/*
 * ExecWorkFile_Create
 *    create a new work file with the specified name, the file type,
 * and the compression type.
 *
 * If this fails, NULL is returned.
 */
ExecWorkFile *
ExecWorkFile_Create(const char *fileName,
					ExecWorkFileType fileType,
					bool delOnClose,
					int compressType)
{
	ExecWorkFile *workfile = NULL;
	void *file = NULL;

	switch(fileType)
	{
		case BUFFILE:
			file = (void *)BufFileCreateFile(fileName, delOnClose, false /* interXact */ );
			break;
		case BFZ:
			file = (void *)bfz_create(fileName, delOnClose, compressType);
			break;
		default:
			ereport(LOG,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("invalid work file type: %d", fileType)));
			Assert(false);
	}

	workfile = palloc0(sizeof(ExecWorkFile));

	workfile->fileType = fileType;
	workfile->compressType = compressType;
	workfile->file = file;
	workfile->fileName = pstrdup(fileName);
	workfile->size = 0;
	ExecWorkfile_SetFlags(workfile);

	return workfile;
}

/*
 * Opens an existing work file with the specified name, the file type,
 * and the compression type.
 *
 * If this fails, NULL is returned.
 */
ExecWorkFile *
ExecWorkFile_Open(const char *fileName,
					ExecWorkFileType fileType,
					bool delOnClose,
					int compressType)
{
	ExecWorkFile *workfile = NULL;
	void *file = NULL;
	int64 file_size = 0;

	switch(fileType)
	{
		case BUFFILE:
			file = (void *)BufFileOpenFile(fileName,
					false, /* Create */
					delOnClose,
					true  /* interXact */ ); /* FIXME WHat should this be? */
			file_size = BufFileGetSize(file);
			break;
		case BFZ:
			file = (void *)bfz_open(fileName, delOnClose, compressType);
			file_size = bfz_totalbytes((bfz_t *)file);
			bfz_scan_begin(file);
			break;
		default:
			ereport(LOG,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("invalid work file type: %d", fileType)));
			Assert(false);
	}

	/* Failed opening existing workfile. Inform the caller */
	if (NULL == file)
	{
		return NULL;
	}

	workfile = palloc0(sizeof(ExecWorkFile));

	workfile->fileType = fileType;
	workfile->compressType = compressType;
	workfile->file = file;
	workfile->fileName = pstrdup(fileName);
	workfile->size = file_size;
	ExecWorkfile_SetFlags(workfile);

	return workfile;
}


/*
 * ExecWorkFile_Write
 *    write the given data from the end of the last write position.
 *
 * This function returns true if the write succeeds. Otherwise, return false.
 */
bool
ExecWorkFile_Write(ExecWorkFile *workfile,
				   void *data,
				   uint64 size)
{
	Assert(workfile != NULL);
	uint64 bytes;

	if (data == NULL || size == 0)
		return false;
	
	WorkfileDiskspace_Reserve(size);

	switch(workfile->fileType)
	{
		case BUFFILE:
			{}
			BufFile *buffile = (BufFile *)workfile->file;

			int64 current_size = BufFileGetSize(buffile);
			bytes = BufFileWrite(buffile, data, size);
			int64 new_size = BufFileGetSize(buffile);
			workfile->size = new_size;

			WorkfileDiskspace_Commit( (new_size - current_size), size);

			if (bytes != size)
				return false;
			break;
		case BFZ:
			bfz_append((bfz_t *)workfile->file, data, size);

			/* bfz_append always adds to the file size */
			workfile->size += size;
			WorkfileDiskspace_Commit(size, size);

			break;
		default:
			insist_log(false, "invalid work file type: %d", workfile->fileType);
	}

	return true;
}

/*
 * ExecWorkFile_Read
 *    read the data with specified size to the given buffer.
 *
 * The given buffer should contain at least the space specified by 
 * 'size'.
 *
 * If the read succeeds, this function returns the number of bytes
 * that are read. Otherwise, returns 0.
 */
uint64
ExecWorkFile_Read(ExecWorkFile *workfile,
				  void *data,
				  uint64 size)
{
	Assert(workfile != NULL);
	uint64 bytes = 0;
	
	switch(workfile->fileType)
	{
		case BUFFILE:
			bytes = BufFileRead((BufFile *)workfile->file, data, size);
			break;
			
		case BFZ:
			bytes = bfz_scan_next((bfz_t *)workfile->file, data, size);
			break;
		default:
			insist_log(false, "invalid work file type: %d", workfile->fileType);
	}
	
	return bytes;
}

/*
 * ExecWorkFile_Rewind
 *    rewind the pointer position to the beginning of the file.
 *
 * This function returns true if this succeeds. Otherwise, return false.
 */
bool
ExecWorkFile_Rewind(ExecWorkFile *workfile)
{
	Assert(workfile != NULL);

	long ret = 0;
	switch(workfile->fileType)
	{
		case BUFFILE:
			ret = BufFileSeek((BufFile *)workfile->file, 0L  /* offset */, SEEK_SET);
			/* BufFileSeek returns 0 if everything went OK */
			return 0 == ret;
		case BFZ:
			bfz_append_end((bfz_t *)workfile->file);
			bfz_scan_begin((bfz_t *)workfile->file);
			break;
		default:
			insist_log(false, "invalid work file type: %d", workfile->fileType);
	}

	return true;
}

/*
 * ExecWorkFile_Tell64
 *    return the value of the current file position indicator.
 */
uint64
ExecWorkFile_Tell64(ExecWorkFile *workfile)
{
	Assert(workfile != NULL);
	uint64 bytes = 0;
	
	switch(workfile->fileType)
	{
		case BUFFILE:
			BufFileTell((BufFile *)workfile->file, (long *) &bytes);
			break;
			
		case BFZ:
			bytes = bfz_totalbytes((bfz_t *)workfile->file);
			break;
		default:
			insist_log(false, "invalid work file type: %d", workfile->fileType);
	}

	return bytes;
}

/*
 * ExecWorkFile_Close
 *    close the work file, and release the space.
 *
 *    Returns the actual size of the file on disk upon closing
 */
int64
ExecWorkFile_Close(ExecWorkFile *workfile)
{
	Assert(workfile != NULL);
	bfz_t *bfz_file = NULL;

	switch(workfile->fileType)
	{
		case BUFFILE:
			BufFileClose((BufFile *)workfile->file);
			break;
			
		case BFZ:
			bfz_file = (bfz_t *)workfile->file;
			Assert(bfz_file != NULL);

			if (bfz_file->mode == BFZ_MODE_APPEND)
			{
				/* Flush data out to disk if we were writing */
				int64 file_size = bfz_append_end(bfz_file);
				/* Adjust the size with WorkfileDiskspace to our actual size */
				ExecWorkfile_AdjustBFZSize(workfile, file_size);
			}

			bfz_close(bfz_file, true);
			break;
		default:
			insist_log(false, "invalid work file type: %d", workfile->fileType);
	}

	int64 size = ExecWorkfile_GetSize(workfile);
	pfree(workfile);

	return size;
}

/*
 * Return the file name for the given workfile.
 * Note that this might have a different meaning, depending on the underlying
 * implementation.
 * For example, BufFile can span over several physical files, and thus this
 * is just a prefix.
 */
char *
ExecWorkFile_GetFileName(ExecWorkFile *workfile)
{
	Assert(workfile != NULL);
	return workfile->fileName;
}

/*
 * ExecWorkFile_Seek
 *   Result is 0 if OK, EOF if not.  Logical position is not moved if an
 *   impossible seek is attempted.
 */
int
ExecWorkFile_Seek(ExecWorkFile *workfile, uint64 offset, int whence)
{
	Assert(workfile != NULL);
	Assert((workfile->flags & EXEC_WORKFILE_RANDOM_ACCESS) != 0);
	int result = 0;

	/* Determine if this seeks beyond EOF and adds disk size */
	int64 additional_size = 0;
	switch (whence)
	{
		case SEEK_SET:
			if (offset > workfile->size)
			{
				additional_size = offset - workfile->size;
			}
			break;

		case SEEK_CUR:
			if (ExecWorkFile_Tell64(workfile) + offset > workfile->size)
			{
				additional_size = ExecWorkFile_Tell64(workfile) + offset - workfile->size;
			}
			break;

		default:
			elog(LOG, "invalid whence: %d", whence);
			Assert(false);
			return EOF;
	}

	/* Reserve disk space if needed */
	if (additional_size > 0)
	{
		WorkfileDiskspace_Reserve(additional_size);
	}

	/* Do the actual seek */
	switch(workfile->fileType)
	{
	case BUFFILE:
		result = BufFileSeek((BufFile *)workfile->file, offset, whence);
		if (additional_size > 0)
		{
			workfile->size = BufFileGetSize((BufFile *)workfile->file);
		}
		break;
	default:
		insist_log(false, "invalid work file type: %d", workfile->fileType);
	}

	if (additional_size > 0)
	{
		WorkfileDiskspace_Commit(additional_size, additional_size);
	}


	return result;
}

void
ExecWorkfile_Flush(ExecWorkFile *workfile)
{
	Assert(workfile != NULL);
	switch(workfile->fileType)
	{
	case BUFFILE:
		BufFileFlush((BufFile *) workfile->file);
		break;
	default:
		insist_log(false, "invalid work file type: %d", workfile->fileType);
	}
}

/*
 * Suspend a file without closing it. For bfz, which allocates a buffer for
 * each open a file, this frees up that buffer but keeps the fd so we can
 * re-open this file later
 *
 * Returns the actual size of the file on disk
 */
int64
ExecWorkfile_Suspend(ExecWorkFile *workfile)
{
	Assert(workfile != NULL);
	Assert((workfile->flags & EXEC_WORKFILE_SUSPENDABLE) != 0);

	int64 size = -1;
	switch(workfile->fileType)
	{
	case BFZ:
		size = bfz_append_end((bfz_t *) workfile->file);
		ExecWorkfile_AdjustBFZSize(workfile, size);
		break;
	default:
		insist_log(false, "invalid work file type: %d", workfile->fileType);
	}
	return size;
}

/*
 * Re-open a suspended file for reading. This allocates all the necessary
 * buffers and data structures to restart reading from the file
 */
void
ExecWorkfile_Restart(ExecWorkFile *workfile)
{
	Assert(workfile != NULL);
	Assert((workfile->flags & EXEC_WORKFILE_SUSPENDABLE) != 0);

	switch(workfile->fileType)
	{
	case BFZ:
		bfz_scan_begin((bfz_t *) workfile->file);
		break;
	default:
		insist_log(false, "invalid work file type: %d", workfile->fileType);
	}
}

/*
 * Returns the size of the underlying file, as tracked by this API
 */
int64
ExecWorkfile_GetSize(ExecWorkFile *workfile)
{
	return workfile->size;
}

/*
 * For a new workfile, sets the capabilities flags according to
 * the known underlying file type capabilities.
 */
static void
ExecWorkfile_SetFlags(ExecWorkFile *workfile)
{
	Assert(workfile != NULL);

	switch(workfile->fileType)
	{

	case BUFFILE:
		workfile->flags |= EXEC_WORKFILE_RANDOM_ACCESS;
		break;
	case BFZ:
		workfile->flags |= EXEC_WORKFILE_SUSPENDABLE;
		break;
	default:
		insist_log(false, "invalid work file type: %d", workfile->fileType);
	}
}

/*
 * Correct the size for a BFZ file when we are done appending to it.
 *
 * During writing to a BFZ, the amount of bytes writen to disk differs from
 * the bytes passed in to write. We can have one of two situations:
 *  - BFZ is compressed. The size on disk is smaller than the bytes written
 *  - BFZ is uncompressed, and we're using checksumming. The size on disk is
 *    slightly larger than the bytes written
 *  Make the necessary correction here.
 */
static void
ExecWorkfile_AdjustBFZSize(ExecWorkFile *workfile, int64 file_size)
{
	Assert(workfile != NULL);

#if USE_ASSERT_CHECKING
	bfz_t *bfz_file = (bfz_t *) workfile->file;
#endif

	if (file_size <= workfile->size)
	{
		/* Actual size on disk is smaller than expected, it must be because it's compressed */
		Assert(bfz_file->compression_index > 0);
		WorkfileDiskspace_Commit(file_size, workfile->size);
	}
	else
	{
		int64 extra_bytes = file_size - workfile->size;
		/* Actual file on disk is bigger than expected. It must be because of checksumming uncompressed file */
		Assert(bfz_file->has_checksum);
		Assert(bfz_file->compression_index == 0);
		/*
		 * We under-reserved disk space in this case. Let's assert that it's only from checksumming.
		 * We add one word of checksum for every BFZ_BUFFER_SIZE (16k) bytes.
		 * That amounts to roughly 0.0244% additional disk size.
		 */
		Assert(extra_bytes < 0.0003 * workfile->size);

		WorkfileDiskspace_Reserve(extra_bytes);
		WorkfileDiskspace_Commit(extra_bytes, extra_bytes);
	}
	workfile->size = file_size;
}
/* EOF */
