/*
 * The Metapa CDB-Loader.
 *
 * Copyright (C) 2005 Metapa Inc
 * All Rights Reserved.
 *
 */
package com.metapa.cdb.loader;

import junit.framework.TestCase;

import java.io.FileInputStream;
import java.io.FileNotFoundException;
import java.io.File;
import java.io.IOException;

/**
 * Unit tests for the AsyncNoOpWriter class.
 *
 * @author alongoldshuv
 *         Date: Jan 6, 2005
 *         Time: 10:09:12 AM
 * @version $Revision$
 */
public class AsyncNoOpWriterTest extends TestCase
{

    /**
     * the segment DB that this writer is assigned to.
     */
    private CdbLoader.SegmentDB segDB;

    /**
     * constructor
     */
    public AsyncNoOpWriterTest()
    {
        super();
    }

    /**
     * test setup:
     * - get parent directory.
     * - open test data file.
     * - create a new segment database.
     */
    public void setUp()
    {

        /**
         * instantiate a segment db. all parameter values are not important.
         */
        CdbLoader loader = new CdbLoader("localhost", 10000, "mydb", "user", "pass");
        segDB = loader.new SegmentDB(1, "some/data/dir", true, true, "localhost", 10000, "mydb", "user", "password", false);
    }

    public void tearDown()

    {
    }

    public void testRowCounts()
    {
        AsyncWriter noOpWriter = new AsyncNoOpWriter(segDB);

        byte[] dataRow = "This is a test data row".getBytes();
        int rowsToWrite = 150;
        long bytesToWrite = dataRow.length * (long)rowsToWrite;

        /**
         * write the data in batches
         */
        for (int i = 0; i < rowsToWrite; i++)
        {
            noOpWriter.writeRow(dataRow, 0, dataRow.length);

            //commit batch after each 25 rows
            if (rowsToWrite % 25 == 0)
            {
                noOpWriter.finishBatch();
                noOpWriter.commitBatch();
            }
        }

        //do a final commit of data.
        noOpWriter.finishBatch();
        noOpWriter.commitBatch();

        /**
         * check that the correct number of rows and bytes are recorded
         */
        assertEquals("Testing total row count", rowsToWrite, noOpWriter.getTotalRowsWritten());
        assertEquals("Testing total bytes count", bytesToWrite, noOpWriter.getTotalBytesWritten());
    }

}
