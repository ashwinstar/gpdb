/*
 * The Metapa CDB-Loader.
 *
 * Copyright (C) 2005 Metapa Inc
 * All Rights Reserved.
 *
 */
package com.metapa.cdb.loader;

import junit.framework.TestCase;
import com.metapa.commons.utils.PropertiesInitializerException;
import com.metapa.commons.utils.Globals;

import java.net.URL;

/**
 * @author alongoldshuv
 *         Date: Jan 6, 2005
 *         Time: 11:47:21 AM
 * @version $Revision$
 */
public class CdbLoaderPropertiesTest extends TestCase
{

    public CdbLoaderPropertiesTest()
    {
         super();
    }

    /**
     * test valid property values.
     */
    public void testValidPropertyValues()
    {
        String[] args = {"-hQA1","-p10005","-s","-b1000","-dmydb","-fcopy.ctl","-umyname",
                         "-Pmypassword","-n","-t","--skipheader=5","-i","--help","--autoescape",
                         "--badfile=test.bad","--logfile=test.log","--noreject","-l8","--failintervals=10"};
        /**
         * initialize command line properties
         */
        CdbLoaderProperties sp = new CdbLoaderProperties();

        try
        {
            sp.initializeProperties(args);
        }
        catch (PropertiesInitializerException e)
        {
            fail("Caught undesireable exception: " + e.getMessage());
        }

        /**
         *  get commandline properties
         */
        String cdbHeadHostname = (String) Globals.getProperty(CdbLoaderProperties.CDB_HEAD_HOSTNAME);
        Integer cdbHeadPort = (Integer) Globals.getProperty(CdbLoaderProperties.CDB_HEAD_PORT);
        String cdbHeadDatabase = (String) Globals.getProperty(CdbLoaderProperties.CDB_HEAD_DATABASE);
        URL loaderControlFile = (URL) Globals.getProperty(CdbLoaderProperties.CDB_LOADER_CONTROL_FILE);
        String userName = (String) Globals.getProperty(CdbLoaderProperties.CDB_USER);
        String password = (String) Globals.getProperty(CdbLoaderProperties.CDB_PASSWORD);
        Integer batchSize = (Integer) Globals.getProperty(CdbLoaderProperties.CDB_BATCH_SIZE);
        Boolean displayOnly = (Boolean) Globals.getProperty(CdbLoaderProperties.CDB_DISPLAY_ONLY);
        Boolean loadWithDisplay = (Boolean) Globals.getProperty(CdbLoaderProperties.CDB_LOAD_WITH_DISPLAY);
        Integer skipHeader = (Integer) Globals.getProperty(CdbLoaderProperties.CDB_SKIP_HEADER);
        Boolean sqlDump = (Boolean) Globals.getProperty(CdbLoaderProperties.CDB_SQL_DUMP);
        Boolean useInserts = (Boolean) Globals.getProperty(CdbLoaderProperties.CDB_USE_INSERTS);
        Boolean helpText = (Boolean) Globals.getProperty(CdbLoaderProperties.CDB_HELP);
        Boolean version = (Boolean) Globals.getProperty(CdbLoaderProperties.CDB_LOADER_VERSION);
        Integer failIntervals = (Integer) Globals.getProperty(CdbLoaderProperties.CDB_NODE_FAILURE_TIMER);
        String logFileName = (String) Globals.getProperty(CdbLoaderProperties.CDB_LOG_FILE);
        String badFileName = null;
        badFileName = (String) Globals.getProperty(CdbLoaderProperties.CDB_BAD_FILE);
        Boolean noReject = (Boolean) Globals.getProperty(CdbLoaderProperties.CDB_NO_REJECT);
        Boolean autoEscape = (Boolean) Globals.getProperty(CdbLoaderProperties.CDB_AUTO_ESCAPE);
        Integer batchLimit = (Integer) Globals.getProperty(CdbLoaderProperties.CDB_BATCH_LIMIT);

        assertEquals("Testing host name", "QA1", cdbHeadHostname);
        assertEquals("Testing port number", 10005, cdbHeadPort.intValue());
        assertTrue("Testing sql dump", sqlDump.booleanValue());
        assertEquals("Testing batch size", 1000, batchSize.intValue());
        assertEquals("Testing db name", "mydb", cdbHeadDatabase);
        assertEquals("Testing user name", "myname", userName);
        assertEquals("Testing password", "mypassword", password);
        assertTrue("Testing no op mode", displayOnly.booleanValue());
        assertTrue("Testing deprecated display with stats mode", loadWithDisplay.booleanValue());
        assertEquals("Testing skip headers", 5 , skipHeader.intValue());
        assertTrue("Testing inserts", useInserts.booleanValue());
        assertTrue("Testing help text", helpText.booleanValue());
        assertTrue("Testing autoescape", autoEscape.booleanValue());
        assertEquals("Testing badfile name","test.bad",badFileName);
        assertEquals("Testing logfile name","test.log",logFileName);
        assertTrue("Testing no reject", noReject.booleanValue());
        assertEquals("Testing batch limit", 8 , batchLimit.intValue());
        assertEquals("Testing fail intervals", 10, failIntervals.intValue());
    }

    /**
     * test property invalid values.
     */
    public void testInvalidPropertyValues()
    {

        checkInvalidValue("-p99999");
        checkInvalidValue("--skipheader=-1");
        checkInvalidValue("-l-1");
        //todo: enhance this list...
    }

    /**
     * fail the test is an exception was NOT thrown.
     * @param arg a single command line parameter.
     */
    public void checkInvalidValue(String arg)
    {
        boolean exceptionCaught = false;

        CdbLoaderProperties sp = new CdbLoaderProperties();

        try
        {
            String[] args = {arg};
            sp.initializeProperties(args);
        }
        catch (Exception e)
        {
            exceptionCaught = true;
            e.getMessage();
        }

        if(!exceptionCaught)
            fail("Passed an invalid value " + arg);

    }

}
