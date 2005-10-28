// ====================================================================
//
//   $Header$
//
//   Copyright (c) 2004, Metapa, Inc.  All rights reserved.
//
//   This software and documentation and the system to which it applies
//   is confidential and proprietary information of Metapa, Inc.
//   ("Confidential Information").  You shall not disclose such
//   Confidential Information and shall use it only in accordance with
//   the terms of the license, consulting, or employment agreement you
//   entered into with Metapa.
//
//
//   Restricted Rights Notice
//
//   Use, duplication, or disclosure by the U.S. Government is subject
//   to restrictions of FAR 52.227-14(g)(2)(6/87) and
//   FAR 52.227-19(6/87), or DFAR 252.227-7015(b)(6/95) and
//   DFAR 227.7202-3(a).
//
// ====================================================================
package com.metapa.test.adminschema;


import com.metapa.commons.pgutils.PgUtilities;
import junit.framework.TestCase;

import java.sql.Connection;
import java.sql.DriverManager;
import java.sql.ResultSet;
import java.sql.Statement;

/**
 * The <code>TestSchema</code> class provides ...
 *
 * NOTE: each test currently does not clean up after itself.  the tests are written such that
 * the previous ones don't affect the current one.  But if you add a new test, you might
 * have problems with this.
 *
 * @author  jeshleman@metapa.com
 * @version 2.0
 * @since   2.0
 */
public class TestSchema extends TestCase
{

    ///////////////
    //Constants
    ///////////////
    public static final int expectedSchemaVersion = 1;


    public static final String[] TABLES_AND_VIEWS = { "cdb", "cdb_get_segdbs", "extent", "host", "listener", "policy",
                                                      "segdb", "segment", "spacecategory", "tablespace", "version" };
    ///////////////
    //Static Fields
    ///////////////


    private Connection conn;

    ///////////////
    //Member Fields
    ///////////////


    protected void setUp() throws Exception
    {
        //load the pg jdbc driver
        try
        {
            Class.forName("org.postgresql.Driver");
        }
        catch (ClassNotFoundException e)
        {
            System.out.println("Can't find Postgres JDBC Driver");
        }

        conn = DriverManager.getConnection( PgUtilities.getJdbcUrl() );
    }

    protected void tearDown() throws Exception
    {

        if( conn != null )
        {
            try{ conn.close(); } catch( Exception e ) { };
        }
        
    }

    ////////////////
    //Static Methods
    ////////////////



    ////////////////
    //Methods
    ////////////////

    public void testSchemaVersion() throws Exception
    {
        /* this kind of an evil thing to go.  but this way, anytime someone increments the schema version
         * number they are gonna have to come back here and make sure that they add/modify any of
         * of these tests.
         */

        Statement stmt = conn.createStatement();
        ResultSet rs = stmt.executeQuery("SELECT * FROM mpp.version");
        rs.next();
        int vers = rs.getInt("schemaversion");


        assertEquals("Unexpected value for schemaversion:", expectedSchemaVersion, vers);

        rs.close();
        stmt.close();

    }

    //go thru and test that all of the schema elements are there
    public void testSchemaExists() throws Exception
    {
        Statement stmt = conn.createStatement();
        ResultSet rs = stmt.executeQuery("SELECT * FROM information_schema.tables WHERE table_schema = 'cdb' ORDER BY table_name");
        int count = 0;
        while( rs.next() )
        {
            String tableName = rs.getString("table_name");
            assertEquals("Unexpected table_name:", TABLES_AND_VIEWS[count], tableName);
            count++;
        }


        assertEquals("Unexpected number of cdb tables and views.", TABLES_AND_VIEWS.length, count);

        rs.close();
        stmt.close();
    }

    /**
     * do a test scenario of calling the cdb admin schema's population functions and make
     * sure the result is what is expected.
     *
     * @throws Exception
     */
    public void testSamplePopulation() throws Exception
    {


        /*
            select * from mpp.add_host( 'localhost', 2211 );
            select * from mpp.add_host( 'bubs', 6666 );
            select * from mpp.add_segment( 'seg1' );
            select * from mpp.add_segment( 'seg2');
            select * from mpp.add_listener( 'localhost', 5000, '/foo');
            select * from mpp.add_listener( 'bubs', 5001, '/foo');

            select * from mpp.add_segment_database( 'seg1', 'localhost', 5000, 't' );
            select * from mpp.add_segment_database( 'seg1', 'bubs', 5001, 'f' );
            select * from mpp.add_segment_database( 'seg2', 'localhost', 5000, 't' );

            select * from mpp.cdb_set_identity( 'mycdb', 'jeshleman', 'secret', 2 , 'localhost', 5000, 'seg1', 't' );

         */

        Statement stmt = conn.createStatement();
        ResultSet rs = stmt.executeQuery("SELECT * from mpp.add_host( 'localhost' ,2200 ) ");
        rs = stmt.executeQuery("SELECT * FROM mpp.add_segment( 'seg1' )");
        rs = stmt.executeQuery("SELECT * FROM mpp.add_segment( 'seg2' )");
        rs = stmt.executeQuery("SELECT * FROM mpp.add_listener( 'localhost', 5000, '/foo' )");
        rs = stmt.executeQuery("SELECT * FROM mpp.add_segment_database( 'seg1', 'localhost', 5000, 't' )");
        rs = stmt.executeQuery("SELECT * FROM mpp.add_segment_database( 'seg2', 'localhost', 5000, 't' )");
        String username = System.getProperty("user.name");
        rs = stmt.executeQuery("SELECT * FROM mpp.cdb_set_identity( 'mycdb', '" + username + "', 'secret', 2, 'localhost', 5000, 'seg1', 't' )");

        rs = stmt.executeQuery("select * from mpp.cdb_get_segdbs");
        rs.next();

        //1st row should be:
        assertEquals("wrong value for dbname", "seg1", rs.getString("dbname"));
        assertEquals("wrong value for isprimary", "t", rs.getString("isprimary"));
        assertEquals("wrong value for hostname", "localhost", rs.getString("hostname"));
        assertEquals("wrong value for port", "5000", rs.getString("port"));
        assertEquals("wrong value for datadir", "/foo", rs.getString("datadir"));
        assertEquals("wrong value for isvalid", "t", rs.getString("isvalid"));


        //2nd row should be:
        rs.next();
        assertEquals("wrong value for dbname", "seg2", rs.getString("dbname"));
        assertEquals("wrong value for isprimary", "t", rs.getString("isprimary"));
        assertEquals("wrong value for hostname", "localhost", rs.getString("hostname"));
        assertEquals("wrong value for port", "5000", rs.getString("port"));
        assertEquals("wrong value for datadir", "/foo", rs.getString("datadir"));
        assertEquals("wrong value for isvalid", "t", rs.getString("isvalid"));


        rs.close();
    }


    /**
     * test the 2 functions that are used to populate the policy table.
     *
     * @throws Exception
     */
    public void testPolicyFunctions() throws Exception
    {
        Statement stmt = conn.createStatement();
        stmt.execute("CREATE TABLE FOO1 ( col1 int )");
        stmt.execute("CREATE TABLE FOO2 ( col1 int )");

        ResultSet rs = stmt.executeQuery("SELECT * from mpp.replicate_table('foo1') ");
        rs = stmt.executeQuery("SELECT * from mpp.partition_table('foo2', '{1}' ) ");

        rs = stmt.executeQuery("SELECT * From mpp.policy");

        rs.next();

        //1st row should be
        assertEquals("wrong value for globalid", "1", rs.getString("globalid"));
        //testing local oids
        //assertEquals("wrong value for localoid", "17368", rs.getString("localoid"));
        assertEquals("wrong value for policytype", "r", rs.getString("policytype"));
        assertEquals("wrong value for attrnums", null, rs.getString("attrnums"));

        //2nd row should be
        rs.next();
        assertEquals("wrong value for globalid", "2", rs.getString("globalid"));
        //assertEquals("wrong value for localoid", "17370", rs.getString("localoid"));
        assertEquals("wrong value for policytype", "p", rs.getString("policytype"));
        assertEquals("wrong value for attrnums", "{1}", rs.getString("attrnums"));


        rs.close();
    }
    ///////////////
    //Inner Classes
    ///////////////


}

// ====================================================================
//
// $Log: TestSchema.java,v $
// Revision 1.1  2005/01/19 20:04:19  jeshleman
// Moved admin schema tests under tests/junit.
//
// Revision 1.2  2004/12/21 23:09:34  mohit
// Updated to use pgutils from cdb-commons project.
//
// Revision 1.1  2004/12/01 08:31:34  jeshleman
// Initial cut at unit testing for the Admin Schema.
//
//
//
// ====================================================================
