/*
 * The Metapa CDB-Loader.
 *
 * Copyright (C) 2005 Metapa Inc
 * All Rights Reserved.
 *
 */
package com.metapa.test.hash;

import junit.framework.*;
import junit.framework.TestCase;

import java.sql.Driver;
import java.sql.DriverManager;
import java.sql.*;
import java.util.Random;
import java.util.Properties;
import java.io.InputStream;

/**
 * @author alongoldshuv
 *         Date: Mar 1, 2005
 *         Time: 4:01:31 PM
 * @version $Revision$
 */
public class HashCorrectnessTest extends TestCase
{
    private Connection conn;

    private int buckets = 0;    //default bad value.

    private int algorithm = 0; //default bad value. Valid values are : 1- FNV1, 2 - FNV1A

    public static final int TOTAL_ALGORITHMS = 2; //for now using FNV1 or FNV1A.

    /**
     * constructor
     */
    public HashCorrectnessTest()
    {
        super();
    }

    /**
     * connect to the database and create (load) the test functions that we will need to use.
     * @throws Exception
     */
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

        //conn = DriverManager.getConnection( PgUtilities.getJdbcUrl() );
        conn = DriverManager.getConnection("jdbc:postgresql://localhost:10000/segDB0?user=alongoldshuv");

        //switch to cdb_role = utility for this connection
        Statement stmt = conn.createStatement();
        stmt.executeUpdate("set cdb_role=utility");
        stmt.close();

        /**
         * retreive other test information
         */
        buckets = Integer.parseInt(props.getProperty("hash.numbuckets"));

        inStr.close();

    }

    protected void tearDown() throws SQLException
    {

        if (conn != null)
        {
            conn.close();
        }

    }

    /**
     * Test that verifies that hashing on the same value but different integer type will
     * always yield the same result accross all types.
     * For example, the following needs always to be true:
     * hashint2(45) = hashint4(45) = hashint8(45)
     *
     * @throws SQLException for any test database connection problem.
     */
    public void testHashIntegers() throws SQLException
    {
        Random numGen = new Random();

        Statement createFuncStmt = conn.createStatement();

        /**
         * Load our test function from libcdbtest into the database.
         */
        createFuncStmt.executeUpdate("CREATE OR REPLACE FUNCTION cdbhash_1_int(integer, integer, integer) RETURNS integer " +
                                     "AS 'libcdbtest.so', 'HASHAPI_Hash_1_Int' " +
                                     "LANGUAGE C IMMUTABLE STRICT");

        createFuncStmt.executeUpdate("CREATE OR REPLACE FUNCTION cdbhash_1_bigint(integer, integer, bigint) RETURNS integer " +
                                     "AS 'libcdbtest.so', 'HASHAPI_Hash_1_BigInt' " +
                                     "LANGUAGE C IMMUTABLE STRICT");

        createFuncStmt.executeUpdate("CREATE OR REPLACE FUNCTION cdbhash_1_smallint(integer, integer, integer) RETURNS integer " +
                                     "AS 'libcdbtest.so', 'HASHAPI_Hash_1_SmallInt' " +
                                     "LANGUAGE C IMMUTABLE STRICT");

        PreparedStatement int16Pstmt = conn.prepareStatement("SELECT cdbhash_1_smallint(?,?,?)");
        PreparedStatement int32Pstmt = conn.prepareStatement("SELECT cdbhash_1_int(?,?,?)");
        PreparedStatement int64Pstmt = conn.prepareStatement("SELECT cdbhash_1_bigint(?,?,?)");

        /**
         * Test with all available hashing altorithms
         */
        for (int algo = 1; algo <= TOTAL_ALGORITHMS; algo++)
        {
            /**
             * Test for 10 different random keys
             */
            for (int i = 0; i < 10; i++)
            {
                /**
                 * Generate a random number in the range -32000 to +32000
                 * (We can use any range that fits into smallint: -32768 to +32767)
                 */
                int testkey = numGen.nextInt(64000) - 32000;

                int16Pstmt.setInt(1, buckets);
                int16Pstmt.setInt(2, algo);
                int16Pstmt.setInt(3, testkey);

                ResultSet rs16 = int16Pstmt.executeQuery();
                rs16.next();

                int result16 = rs16.getInt(1);

                int32Pstmt.setInt(1, buckets);
                int32Pstmt.setInt(2, algo);
                int32Pstmt.setInt(3, testkey);

                ResultSet rs32 = int32Pstmt.executeQuery();
                rs32.next();

                int result32 = rs32.getInt(1);

                int64Pstmt.setInt(1, buckets);
                int64Pstmt.setInt(2, algo);
                int64Pstmt.setInt(3, testkey);

                ResultSet rs64 = int64Pstmt.executeQuery();
                rs64.next();

                int result64 = rs64.getInt(1);

                assertEquals("Int2 hashed same as Int4 on identical values with " + (algo == 1 ? "FNV1" : "FNV1A"), result16, result32);
                assertEquals("Int4 hashed same as Int8 on identical values with " + (algo == 1 ? "FNV1" : "FNV1A"), result32, result64);
            }
        }

        createFuncStmt.close();

        int16Pstmt.close();
        int32Pstmt.close();
        int64Pstmt.close();
    }

    /**
     * Tests hashing identical values on different string types, and expecting to get the same results
     * from all of them,
     * For example, the following needs to always be true:
     * hashText("foo") = hashVarchar("foo") = hashBpchar("foo")
     *
     * @throws SQLException
     */
    public void testHashStringTypes() throws SQLException
    {

        Statement createFuncStmt = conn.createStatement();

        /**
         * Load our test function from libcdbtest into the database.
         */
        createFuncStmt.executeUpdate("CREATE OR REPLACE FUNCTION cdbhash_1_bpchar(integer, integer, BpChar) RETURNS integer " +
                                     "AS 'libcdbtest.so', 'HASHAPI_Hash_1_BpChar' " +
                                     "LANGUAGE C IMMUTABLE STRICT");

        createFuncStmt.executeUpdate("CREATE OR REPLACE FUNCTION cdbhash_1_text(integer, integer, text) RETURNS integer " +
                                     "AS 'libcdbtest.so', 'HASHAPI_Hash_1_Text' " +
                                     "LANGUAGE C IMMUTABLE STRICT");

        createFuncStmt.executeUpdate("CREATE OR REPLACE FUNCTION cdbhash_1_varchar(integer, integer, VarChar) RETURNS integer " +
                                     "AS 'libcdbtest.so', 'HASHAPI_Hash_1_Varchar' " +
                                     "LANGUAGE C IMMUTABLE STRICT");

        PreparedStatement bpcharPstmt = conn.prepareStatement("SELECT cdbhash_1_bpchar(?,?,?)");
        PreparedStatement textPstmt = conn.prepareStatement("SELECT cdbhash_1_text(?,?,?)");
        PreparedStatement varcharPstmt = conn.prepareStatement("SELECT cdbhash_1_varchar(?,?,?)");

        String[] testStr = {"a",
                            "a short string",
                            "another string with some trailing blanks      ",
                            "a somewhat longer string, length doesn't have a real significanse here but we still want " +
                            "to know that a long string or a long paragraph hashes to the exact same place whether the " +
                            "data type is text, varchar or blank padded character string... ",
                            "CDB 1234567890!@#$% == ^&*()_+=-  ",
                            "ONE last TEST string TO compare EQUALITY on STRING data TYPES"};
        /**
         * Test with all available hashing altorithms
         */
        for (int algo = 1; algo <= TOTAL_ALGORITHMS; algo++)
        {
            /**
             * Test for all test strings.
             */
            for (int i = 0 ; i < testStr.length ; i++)
            {
                bpcharPstmt.setInt(1, buckets);
                bpcharPstmt.setInt(2, algo);
                bpcharPstmt.setString(3, testStr[i]);

                ResultSet rsBpchar = bpcharPstmt.executeQuery();
                rsBpchar.next();

                int resultBpchar = rsBpchar.getInt(1);

                textPstmt.setInt(1, buckets);
                textPstmt.setInt(2, algo);
                textPstmt.setString(3, testStr[i]);

                ResultSet rsText = textPstmt.executeQuery();
                rsText.next();

                int resultText = rsText.getInt(1);

                varcharPstmt.setInt(1, buckets);
                varcharPstmt.setInt(2, algo);
                varcharPstmt.setString(3, testStr[i]);

                ResultSet rsVarchar = varcharPstmt.executeQuery();
                rsVarchar.next();

                int resultVarchar = rsVarchar.getInt(1);

                assertEquals("Bpchar hashed same as Text on identical values with " + (algo == 1 ? "FNV1" : "FNV1A"), resultBpchar, resultText);
                assertEquals("Text hashed same as Varchar on identical values with " + (algo == 1 ? "FNV1" : "FNV1A"), resultText, resultVarchar);
            }
        }

        createFuncStmt.close();

        bpcharPstmt.close();
        textPstmt.close();
        varcharPstmt.close();
    }

    /**
     * Test that trailing blanks on a string doesn't affect the hash result.
     * @throws SQLException
     */
    public void testHashStringTrailingBlanks() throws SQLException
    {

        Statement createFuncStmt = conn.createStatement();

        /**
         * Load our test function from libcdbtest into the database.
         */
        createFuncStmt.executeUpdate("CREATE OR REPLACE FUNCTION cdbhash_1_bpchar(integer, integer, BpChar) RETURNS integer " +
                                     "AS 'libcdbtest.so', 'HASHAPI_Hash_1_BpChar' " +
                                     "LANGUAGE C IMMUTABLE STRICT");

        createFuncStmt.executeUpdate("CREATE OR REPLACE FUNCTION cdbhash_1_text(integer, integer, text) RETURNS integer " +
                                     "AS 'libcdbtest.so', 'HASHAPI_Hash_1_Text' " +
                                     "LANGUAGE C IMMUTABLE STRICT");

        createFuncStmt.executeUpdate("CREATE OR REPLACE FUNCTION cdbhash_1_varchar(integer, integer, VarChar) RETURNS integer " +
                                     "AS 'libcdbtest.so', 'HASHAPI_Hash_1_Varchar' " +
                                     "LANGUAGE C IMMUTABLE STRICT");

        PreparedStatement bpcharPstmt = conn.prepareStatement("SELECT cdbhash_1_bpchar(?,?,?)");
        PreparedStatement textPstmt = conn.prepareStatement("SELECT cdbhash_1_text(?,?,?)");
        PreparedStatement varcharPstmt = conn.prepareStatement("SELECT cdbhash_1_varchar(?,?,?)");

        String[] testStr =       {"a",
                                  "another string with some trailing blanks"};
        String[] testBlnkStr =   {"a  ",
                                  "another string with some trailing blanks                       "};

        /**
         * Test with all available hashing altorithms
         */
        for (int algo = 1; algo <= TOTAL_ALGORITHMS; algo++)
        {
            /**
             * Test for all test strings.
             */
            for (int i = 0 ; i < testStr.length ; i++)
            {
                ////////////////////////
                // BPCHAR/CHAR
                ////////////////////////

                //WITHOUT TRAILING BLANKS
                //
                bpcharPstmt.setInt(1, buckets);
                bpcharPstmt.setInt(2, algo);
                bpcharPstmt.setString(3, testStr[i]);

                ResultSet rsBpchar1 = bpcharPstmt.executeQuery();
                rsBpchar1.next();

                int resultBpchar1 = rsBpchar1.getInt(1);

                //WITH TRAILING BLANKS
                //
                bpcharPstmt.setInt(1, buckets);
                bpcharPstmt.setInt(2, algo);
                bpcharPstmt.setString(3, testBlnkStr[i]);

                ResultSet rsBpchar2 = bpcharPstmt.executeQuery();
                rsBpchar2.next();

                int resultBpchar2 = rsBpchar2.getInt(1);

                assertEquals("A BPCHAR string hashes to the same bucket whether or not it has one of more" +
                             " trailing blanks (using " + (algo == 1 ? "FNV1" : "FNV1A") + ")", resultBpchar1, resultBpchar2);



                 ////////////////////////
                 // TEXT
                 ////////////////////////

                 //WITHOUT TRAILING BLANKS
                 //
                 textPstmt.setInt(1, buckets);
                 textPstmt.setInt(2, algo);
                 textPstmt.setString(3, testStr[i]);

                 ResultSet rsText1 = textPstmt.executeQuery();
                 rsText1.next();

                 int resultText1 = rsText1.getInt(1);

                 //WITH TRAILING BLANKS
                 //
                 textPstmt.setInt(1, buckets);
                 textPstmt.setInt(2, algo);
                 textPstmt.setString(3, testBlnkStr[i]);

                 ResultSet rsText2 = textPstmt.executeQuery();
                 rsText2.next();

                 int resultText2 = rsText2.getInt(1);

                 assertEquals("A TEXT string hashes to the same bucket whether or not it has one of more" +
                              " trailing blanks (using " + (algo == 1 ? "FNV1" : "FNV1A") + ")", resultText1, resultText2);

                 ////////////////////////
                 // VARCHAR
                 ////////////////////////

                 //WITHOUT TRAILING BLANKS
                 //
                 varcharPstmt.setInt(1, buckets);
                 varcharPstmt.setInt(2, algo);
                 varcharPstmt.setString(3, testStr[i]);

                 ResultSet rsVarchar1 = varcharPstmt.executeQuery();
                 rsVarchar1.next();

                 int resultVarchar1 = rsVarchar1.getInt(1);

                 //WITH TRAILING BLANKS
                 //
                 varcharPstmt.setInt(1, buckets);
                 varcharPstmt.setInt(2, algo);
                 varcharPstmt.setString(3, testBlnkStr[i]);

                 ResultSet rsVarchar2 = varcharPstmt.executeQuery();
                 rsVarchar2.next();

                 int resultVarchar2 = rsVarchar2.getInt(1);

                 assertEquals("A VARCHAR string hashes to the same bucket whether or not it has one of more" +
                              " trailing blanks (using " + (algo == 1 ? "FNV1" : "FNV1A") + ")", resultVarchar1, resultVarchar2);

            }
        }

        createFuncStmt.close();

        bpcharPstmt.close();
        textPstmt.close();
        varcharPstmt.close();
    }



}
