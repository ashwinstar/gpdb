/*
 * The Metapa CDB-Loader.
 *
 * Copyright (C) 2005 Metapa Inc
 * All Rights Reserved.
 *
 */
package com.metapa.cdb.loader;

import junit.framework.*;

import java.io.IOException;
import java.util.ArrayList;

/**
 * Tests the various outcomes of parsing valid and non valid load commands.
 * 
 * @author alongoldshuv
 *         Date: Jan 5, 2005
 *         Time: 4:35:45 PM
 * @version $Revision$
 */
public class PgUtilsTest extends TestCase
{
    private PgUtils.PgCopy copy;

    public PgUtilsTest()
    {
        super();
    }

    /**
     * test a valid copy command for correct parsing of its parameters, and correct
     * default values.
     *
     * @throws IOException
     * @throws ParsePgCopyException
     */
    public void testValidParams() throws IOException, ParsePgCopyException
    {
        String copyCmd = "COPY myschema.mytable from 'mydata.dat' WITH DELIMITER '|' NULL '@' ESCAPE '*';";

        System.out.println("Copy command being tested: " + copyCmd);

        copy = PgUtils.parsePgCopy(copyCmd);

        assertEquals("testing table name ", "mytable", copy.getTableName());
        assertEquals("testing schema name ", "myschema", copy.getSchemaName());
        assertEquals("testing data file name ", "'mydata.dat'", copy.getFileName());
        assertEquals("testing delimiter representation ", "'|'", copy.getDelimiter());
        assertEquals("testing null representation ", "'@'", copy.nullName);
        assertEquals("testing escape representation ", "'*'", copy.getEscape());
    }

    /**
     * test a valid copy command for correct parsing of its parameters, and correct
     * default values when values are mixed in order
     *
     * @throws IOException
     * @throws ParsePgCopyException
     */
    public void testValidUnorderedParams() throws IOException, ParsePgCopyException
    {
        String copyCmd = "COPY mytable from 'mydata.dat' WITH ESCAPE '*' DELIMITER AS '|' NULL '@' ;";

        System.out.println("Copy command being tested: " + copyCmd);

        copy = PgUtils.parsePgCopy(copyCmd);

        assertEquals("testing table name ", "mytable", copy.getTableName());
        assertNull(copy.getSchemaName());
        assertEquals("testing data file name ", "'mydata.dat'", copy.getFileName());
        assertEquals("testing delimiter representation ", "'|'", copy.getDelimiter());
        assertEquals("testing null representation ", "'@'", copy.nullName);
        assertEquals("testing escape representation ", "'*'", copy.getEscape());
    }


    /**
     * test a valid copy command for correct parsing of its parameters, and correct
     * default values.
     *
     * @throws IOException
     * @throws ParsePgCopyException
     */
    public void testValidCopyCmdDefaults() throws IOException, ParsePgCopyException
    {
        String copyCmd = "COPY myschema.mytable from STDIN;";

        System.out.println("Copy command being tested: " + copyCmd);

        copy = PgUtils.parsePgCopy(copyCmd);

        assertEquals("testing if from stdin ", true, copy.useStdIn);
        assertEquals("testing escape ", "'\\'", copy.getEscape());
        assertEquals("testing delimiter ", "'\t'", copy.getDelimiter());
        assertEquals("testing for empty column list", 0, copy.getColumnNames().size());
    }

    /**
     * test the behavior of an invalid copy command.
     *
     * @throws IOException
     */
    public void testInvalidCopyCmd() throws IOException
    {
        String copyCmd = "COPY yada yada";
        boolean caughtException = false;

        System.out.println("Copy command being tested: " + copyCmd);

        try
        {
            copy = PgUtils.parsePgCopy(copyCmd);
        }
        catch (ParsePgCopyException ppce)
        {
            System.out.println("Caught expected exception: " + ppce.getMessage());
            caughtException = true;
        }

        if(!caughtException)
            fail("Exception not thrown while parsing this INVALID copy command");

    }

    /**
     * test a valid column list. check the names and the number of columns.
     *
     * @throws IOException
     * @throws ParsePgCopyException
     */
    public void testValidColumnList() throws IOException, ParsePgCopyException
    {
        String copyCmd = "COPY mytable(col1, col2, col3) from STDIN;";

        System.out.println("Copy command being tested: " + copyCmd);

        copy = PgUtils.parsePgCopy(copyCmd);

        ArrayList columns = new ArrayList(0);
        columns = copy.getColumnNames();

        assertEquals("testing number of columns ", 3, columns.size());
        assertEquals("testing for the 1st column", "col1", columns.get(0).toString());
        assertEquals("testing for the 2nd column", "col2", columns.get(1).toString());
        assertEquals("testing for the 3rd column", "col3", columns.get(2).toString());

    }

    /**
     * test an invalid column list. expect an exception to be caught.
     *
     * @throws IOException
     */
    public void testInvalidColumnList() throws IOException
    {
        String copyCmd = "COPY mytable(col1, , col2, col3) from STDIN;";
        boolean caughtException = false;

        System.out.println("Copy command being tested: " + copyCmd);

        try
        {
            copy = PgUtils.parsePgCopy(copyCmd);
        }
        catch (ParsePgCopyException ppce)
        {
            System.out.println("Caught expected exception " + ppce.getMessage());
            caughtException = true;
        }

        if (!caughtException)
            fail("Exception not thrown while parsing this INVALID column list.");

    }


}
