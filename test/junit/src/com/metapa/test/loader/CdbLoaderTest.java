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

    protected void setUp() throws Exception
    {
        /**
         * retreive entry node info from the test properties file
         * and connect to it in utility mode.
         */
        Properties props = new Properties();

        InputStream inStr = getClass().getClassLoader().getResourceAsStream("com/metapa/test/hash/test.properties");
        if (inStr == null)
            throw new Exception("Could not find resource 'test.properties'");

        props.load(inStr);

        String host = props.getProperty("entry.host");
        int port = Integer.parseInt(props.getProperty("entry.port"));
        String dbname = props.getProperty("entry.dbname");
        String username = props.getProperty("entry.user");
        String password = props.getProperty("entry.password");

        inStr.close();
        /**
         * create a loader given the home segment properties.
         */
        //loader = new CdbLoader(host, port, dbname, username, password);
    }

    protected void tearDown()
    {

    }



    //
    //CLUSTER INTO TEST REMOVED AFTER CHANGES TO CDB2 LOADER
    //
    

    public void testInsertAll()
    {
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
    }
}

