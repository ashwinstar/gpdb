/*
 * The Metapa CDB-Loader.
 *
 * Copyright (C) 2004-2005 Metapa Inc
 * All Rights Reserved.
 *
 */
package com.metapa.test.hash;

import junit.framework.TestCase;

import java.sql.*;
import java.util.Random;
import java.util.Properties;
import java.io.FileInputStream;
import java.io.InputStream;

import com.metapa.commons.pgutils.PgUtilities;

/**
 * @author alongoldshuv
 *         Date: Mar 8, 2005
 *         Time: 3:37:31 PM
 * @version $Revision$
 */
public class HashSpreadnessTest extends TestCase
{
    private Connection conn;

    private int buckets = 0;    //default bad value.

    private int numkeys = 0;

    private int algorithm = 0; //default bad value. Valid values are : 1- FNV1, 2 - FNV1A

    public static final int TOTAL_ALGORITHMS = 2; //for now using FNV1 or FNV1A.

    public HashSpreadnessTest()
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

        //load the pg jdbc driver
        try
        {
            Class.forName("org.postgresql.Driver");
        }
        catch (ClassNotFoundException e)
        {
            System.out.println("Can't find Postgres JDBC Driver");
        }

        //connect to the entry node.
        String URL = PgUtilities.createJdbcUrl(host, port, dbname, username, password);
        conn = DriverManager.getConnection(URL);

        //switch to cdb_role = utility for this connection
        Statement stmt = conn.createStatement();
        stmt.executeUpdate("set cdb_role=utility");
        stmt.close();

        /**
         * retreive other test information
         */
        buckets = Integer.parseInt(props.getProperty("hash.numbuckets"));
        numkeys = Integer.parseInt(props.getProperty("hash.numkeys"));


        inStr.close();
    }

    protected void tearDown() throws SQLException
    {

        if (conn != null)
        {
            conn.close();
        }

    }

    public void testHashRandInts() throws SQLException
    {

        Random numGen = new Random();
        int numRange = 2000000;
        int keys[] = new int[numkeys];
        int bucketCount[] = new int[buckets];
        StringBuffer results = new StringBuffer();

        Statement createFuncStmt = conn.createStatement();

        /**
         * Load our test function from libcdbtest into the database.
         */
        createFuncStmt.executeUpdate("CREATE OR REPLACE FUNCTION cdbhash_1_int(integer, integer, integer) RETURNS integer " +
                                     "AS 'libcdbtest.so', 'HASHAPI_Hash_1_Int' " +
                                     "LANGUAGE C IMMUTABLE STRICT");

        PreparedStatement int32Pstmt = conn.prepareStatement("SELECT cdbhash_1_int(?,?,?)");

        results.append("\nSkewness Test: ").append("\nInput keys: 4 byte integers in the range of -");
        results.append(numRange / 2).append(" to +").append(numRange / 2);
        results.append("\nNumber of input keys: ").append(numkeys);
        results.append("\nNumber of buckets: ").append(buckets);


        /**
         * Generate the test values (we want to pre-generate them in order
         * to use the exact same values for all algorithms tested).
         */
        for( int i = 0 ; i < numkeys ; i++)
            keys[i] = numGen.nextInt(numRange) - (numRange / 2);

        /**
         * Test with all available hashing altorithms
         */
        for (int algo = 1; algo <= TOTAL_ALGORITHMS; algo++)
        {
            /**
             * make sure bucket count array is inititalized to 0 counts.
             */
            for (int i = 0; i < bucketCount.length; i++)
                bucketCount[i] = 0;

            /**
             * Test for some random keys...
             */
            for (int i = 0; i < numkeys; i++)
            {

                int32Pstmt.setInt(1, buckets);
                int32Pstmt.setInt(2, algo);
                int32Pstmt.setInt(3, keys[i]);

                ResultSet rs32 = int32Pstmt.executeQuery();
                rs32.next();

                int result = rs32.getInt(1);

                bucketCount[result]++;
            }

            results.append("\n\nAlgorithm used: " + (algo == 1 ? "FNV1" : "FNV1A") + "\n---------------------");

            for (int i = 0; i < bucketCount.length; i++)
                results.append("\nBucket ").append(i).append(": ").append(bucketCount[i]);

            //calculate statistics
            int maxInd = getMaxCountBucket(bucketCount);
            int minInd = getMinCountBucket(bucketCount);
            int avg = getAvgCount(bucketCount);
            float var = getVariance(bucketCount, avg);
            float dev = getDeviation(bucketCount, maxInd, minInd, (float)numkeys);

            //record the minimum and maximum values and their respective buckets.
            results.append("\n\nMax ").append(bucketCount[maxInd]).append(" (bucket ").append(maxInd).append(")");
            results.append("\nMin ").append(bucketCount[minInd]).append(" (bucket ").append(minInd).append(")");
            results.append("\nAvg ").append(avg);
            results.append("\nPercent Diff ").append(((float)bucketCount[maxInd] - (float)bucketCount[minInd])/ avg);
            results.append("\nStd Dev ").append((float)Math.sqrt(var));

        }//for each algo.

        //display the test results
        System.out.println(results.toString());


        //free resources.
        createFuncStmt.close();
        int32Pstmt.close();
    }

    /**
     * get the array index that holds the largest count.
     * @param bCount
     * @return
     */
    public int getMaxCountBucket(int []bCount)
    {
        int maxValue = 0;
        int maxIndex = 0;

        for (int i = 0 ; i < bCount.length ; i++)
        {
            if(bCount[i] > maxValue)
            {
                maxIndex = i;
                maxValue = bCount[i];
            }
        }

        return maxIndex;
    }

    /**
     * get the array index that holds the smallest count.
     * @param bCount
     * @return
     */
    public int getMinCountBucket(int []bCount)
    {
        int minValue = Integer.MAX_VALUE;
        int minIndex = 0;

        for (int i = 0 ; i < bCount.length ; i++)
        {
            if(bCount[i] < minValue)
            {
                minIndex = i;
                minValue = bCount[i];
            }
        }

        return minIndex;
    }

    public int getAvgCount(int []bCount)
    {
        int sum = 0;

        for (int i = 0 ; i < bCount.length ; i++)
            sum +=bCount[i];

        return (sum/bCount.length);
    }

    public float getVariance(int []bCount, int avg)
    {
        float sumOfDevSqrt = 0;

        for (int i = 0 ; i < bCount.length ; i++)
            sumOfDevSqrt += ((bCount[i] - avg) * (bCount[i] - avg));

        return (sumOfDevSqrt/bCount.length);
    }

    public float getDeviation(int []bCount, int maxInd, int minInd, float totalKeys)
    {
         return ((bCount[maxInd] - bCount[minInd])/totalKeys);

    }

}
