/*
 * The Metapa CDB-Loader.
 *
 * Copyright (C) 2004-2005 Metapa Inc
 * All Rights Reserved.
 *
 */
package com.metapa.test.loader;

import junit.framework.*;

import java.io.*;
import java.util.Properties;
import java.sql.SQLException;

import com.metapa.cdb.loader.CdbLoader;

/**
 * Unit tests for the CdbLoader class.
 *
 * @author alongoldshuv
 *         Date: Jan 5, 2005
 *         Time: 2:48:09 PM
 * @version $Revision$ 
 */
public class CdbLoaderTest extends TestCase
{
    /**
     * a CDB Loader object.
     */
    private CdbLoader loader;

    /**
     * the parent directory.
     */
    private String parentDir;

    /**
     * the test data file input stream.
     */
    private FileInputStream fileInStream;


    public CdbLoaderTest()
    {
        super();

    }

    protected void setUp() throws FileNotFoundException, IOException
    {
        /**
         * retreive test properties from system properties (these properties
         * are read from the test properties file and copied to the system
         * properties by the ant runTests target).
         */
        String host = System.getProperty("test.homeseg.host");
        int port = Integer.parseInt(System.getProperty("test.homeseg.port"));
        String dbname = System.getProperty("test.homeseg.dbname");
        String username = System.getProperty("test.homeseg.user");
        String password = System.getProperty("test.homeseg.password");

        /**
         * create a loader given the home segment properties.
         */
        loader = new CdbLoader(host, port, dbname, username, password);
    }

    protected void tearDown()
    {

    }



    //
    //CLUSTER INTO TEST REMOVED AFTER CHANGES TO CDB2 LOADER
    //
    

//    public void testInsertAll()
//    {
//        File dir = new File(".");
//        String dataFile = "test_set1.dat";
//
//
//        /**
//         * get the parent directory
//         */
//        try
//        {
//            parentDir = dir.getCanonicalPath();
//        }
//        catch (Exception e)
//        {
//            System.out.println("Couldn't retreive parent directory string in the test setup. " + e.getMessage());
//        }
//
//        /**
//         * open the test data file.
//         */
//        try
//        {
//            fileInStream = new FileInputStream(parentDir + "/test/data/" + dataFile);
//        }
//        catch (FileNotFoundException e)
//        {
//            System.out.println("Couldn't open the test data file. " + e.getMessage());
//        }
//
//
//        try
//        {
//            fileInStream.close();
//        }
//        catch (IOException e)
//        {
//            System.out.println("Couldn't close the test data file. " + e.getMessage());
//        }
//
//    }
}

