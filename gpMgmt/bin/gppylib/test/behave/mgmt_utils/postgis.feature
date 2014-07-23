# Sample data is from http://workshops.opengeo.org/postgis-intro
@postgis
Feature: postgis regression

    Scenario: check PostGIS 2.0 is installed
        Then verify that file "postgis.sql" exists under "${GPHOME}/share/postgresql/contrib/postgis-2.0"
        Then verify that file "postgis_restore.pl" exists under "${GPHOME}/share/postgresql/contrib/postgis-2.0"

    Scenario: check GPDB and PostGIS version info
        When all rows from table "version()" db "opengeo" are stored in the context
        Then validate that "Greenplum Database" is in the stored rows
        When all rows from table "postgis_full_version()" db "opengeo" are stored in the context
        Then validate that "POSTGIS="2.0.3" is in the stored rows
        And validate that "GEOS" is in the stored rows

    # Basic geometries operations and functions

    Scenario: create and check various geometry types
        When below sql is executed in "opengeo" db
            """
            CREATE TABLE geometries (name varchar, geom geometry)
            """
        And below sql is executed in "opengeo" db
            """
            INSERT INTO geometries VALUES
                ('Point', 'POINT(0 0)'),
                ('Linestring', 'LINESTRING(0 0, 1 1, 2 1, 2 2)'),
                ('Polygon', 'POLYGON((0 0, 1 0, 1 1, 0 1, 0 0))'),
                ('PolygonWithHole', 'POLYGON((0 0, 10 0, 10 10, 0 10, 0 0),(1 1, 1 2, 2 2, 2 1, 1 1))'), 
                ('Collection', 'GEOMETRYCOLLECTION(POINT(2 0),POLYGON((0 0, 1 0, 1 1, 0 1, 0 0)))')
            """
        And all rows from table "(SELECT name, ST_AsText(geom) FROM geometries) AS foo" db "opengeo" are stored in the context
        Then validate that stored rows has "5" lines of output

        When execute following sql in db "opengeo" and store result in the context
            """
            SELECT name, ST_GeometryType(geom), ST_NDims(geom), ST_SRID(geom) FROM geometries
            """
        Then validate that "Collection|ST_GeometryCollection|2|0" "string|string|int|int" seperated by "|" is in the stored rows 
        When execute sql "SELECT ST_AsText(geom) FROM geometries WHERE name = 'Point'" in db "opengeo" and store result in the context
        Then validate that "POINT(0 0)" is in the stored rows
        When execute sql "SELECT ST_X(geom), ST_Y(geom) FROM geometries WHERE name = 'Point'" in db "opengeo" and store result in the context
        Then validate that "0|0" "int|int" seperated by "|" is in the stored rows 
        When execute sql "SELECT ST_Length(geom)::text FROM geometries WHERE name = 'Linestring'" in db "opengeo" and store result in the context
        Then validate that "3.41421356237309" is in the stored rows
        When execute sql "SELECT name, ST_Area(geom) FROM geometries WHERE name LIKE 'Polygon%'" in db "opengeo" and store result in the context
        Then validate that "Polygon|1" "string|int" seperated by "|" is in the stored rows 
        Then validate that "PolygonWithHole|99" "string|int" seperated by "|" is in the stored rows 
        When execute sql "SELECT name, ST_AsText(geom) FROM geometries WHERE name = 'Collection'" in db "opengeo" and store result in the context
        Then validate that "Collection|GEOMETRYCOLLECTION(POINT(2 0),POLYGON((0 0,1 0,1 1,0 1,0 0)))" "string|string" seperated by "|" is in the stored rows 
        When execute sql "SELECT ST_AsEWKT(ST_GeometryFromText('LINESTRING(0 0 0,1 0 0,1 1 2)'))" in db "opengeo" and store result in the context
        Then validate that "LINESTRING(0 0 0,1 0 0,1 1 2)" is in the stored rows
        When execute sql "SELECT 'SRID=4326;POINT(0 0)'::geometry::text" in db "opengeo" and store result in the context
        Then validate that "0101000020E610000000000000000000000000000000000000" is in the stored rows

    Scenario: what is the area of the 'West Village' neighborhood
        When execute following sql in db "opengeo" and store result in the context
            """
            SELECT ST_Area(geom)::text FROM nyc_neighborhoods WHERE name = 'West Village'
            """
        Then validate that "1044614" is in the stored rows

    Scenario: what is the area of Manhattan in acres
        When execute sql "SELECT Sum(ST_Area(geom))::text FROM nyc_neighborhoods WHERE boroname = 'Manhattan'" in db "opengeo" and store result in the context
        Then validate that "56517650" is in the stored rows

    Scenario: how many census blocks in New York City have a hole in them
        When execute following sql in db "opengeo" and store result in the context
            """
            SELECT Count(*)::text FROM nyc_census_blocks WHERE ST_NumInteriorRings(ST_GeometryN(geom,1)) > 0
            """
        Then validate that "45" is in the stored rows

    Scenario: what is the total length of streets (in km) in New York City
        When execute following sql in db "opengeo" and store result in the context
            """
            SELECT (Sum(ST_Length(geom)) / 1000)::text FROM nyc_streets
            """
        Then validate that "10418" is in the stored rows

    Scenario: how long is 'Columbus Cir'
        When execute following sql in db "opengeo" and store result in the context
            """
            SELECT ST_Length(geom)::text FROM nyc_streets WHERE name = 'Columbus Cir'
            """
        Then validate that "308" is in the stored rows

    Scenario: how many polygons are in the 'West Village' multipolygon
        When execute following sql in db "opengeo" and store result in the context
            """
            SELECT ST_NumGeometries(geom)::text FROM nyc_neighborhoods WHERE name = 'West Village'
            """
        Then validate that "1" is in the stored rows

    Scenario: how many polygons are in the 'West Village' multipolygon
        When execute following sql in db "opengeo" and store result in the context
            """
            SELECT type, Round(Sum(ST_Length(geom)))::int AS length FROM nyc_streets GROUP BY type ORDER BY length DESC
            """
        Then validate that following rows are in the stored rows
            | type                       |  length | 
            | residential                | 8629870 |
            | motorway                   |  403622 |
            | tertiary                   |  360395 |
            | motorway_link              |  294261 |
            | secondary                  |  276264 |

    # Spatial relationships 

    Scenario: ST_Equals should work
        When execute following sql in db "opengeo" and store result in the context
            """
            SELECT name FROM nyc_subway_stations 
            WHERE ST_Equals(geom, '0101000020266900000EEBD4CF27CF2141BC17D69516315141')
            """
        Then validate that "Broad St" is in the stored rows

    Scenario: ST_Intersects should work
        When execute following sql in db "opengeo" and store result in the context
            """
            SELECT name, boroname FROM nyc_neighborhoods
            WHERE ST_Intersects(geom, ST_GeomFromText('POINT(583571 4506714)',26918))
            """
        Then validate that "Manhattan" is in the stored rows

    Scenario: ST_Distance should work
        When execute following sql in db "opengeo" and store result in the context
            """
            SELECT ST_Distance(ST_GeometryFromText('POINT(0 5)'), ST_GeometryFromText('LINESTRING(-2 2, 2 2)'))::text;
            """
        Then validate that "3" is in the stored rows

    Scenario: What streets are nearby 'Broad Street' subway station
        When execute following sql in db "opengeo" and store result in the context
            """
            SELECT name FROM nyc_streets
            WHERE ST_DWithin(geom, ST_GeomFromText('POINT(583571 4506714)',26918), 10);
            """
        Then validate that "Wall St" is in the stored rows
        And  validate that "Broad St" is in the stored rows
        And  validate that "Nassau St" is in the stored rows

    Scenario: What is the geometry value for street named 'Atlantic Commons'
        When execute following sql in db "opengeo" and store result in the context
            """
            SELECT ST_AsText(geom) FROM nyc_streets WHERE name = 'Atlantic Commons';
            """
        Then validate that "MULTILINESTRING((586781" is in the stored rows

    Scenario: What neighborhood and borough is Atlantic Commons in
        When execute following sql in db "opengeo" and store result in the context
            """
            SELECT name, boroname FROM nyc_neighborhoods
            WHERE ST_Intersects(geom, ST_GeomFromText('LINESTRING(586782 4504202,586864 4504216)', 26918))
            """
        Then validate that "Fort Green|Brooklyn" "string|string" seperated by "|" is in the stored rows 

    Scenario: What streets does Atlantic Commons join with
        When execute following sql in db "opengeo" and store result in the context
            """
            SELECT name FROM nyc_streets
            WHERE ST_DWithin(geom, ST_GeomFromText('LINESTRING(586782 4504202,586864 4504216)', 26918), 1)
            """
        Then validate that "S Oxford St" is in the stored rows
        And  validate that "Cumberland St" is in the stored rows
    
     Scenario: How many people live on (within 50 meters of) Atlantic commons
        When execute following sql in db "opengeo" and store result in the context
            """
            SELECT Sum(popn_total)::text FROM nyc_census_blocks
            WHERE ST_DWithin(geom, ST_GeomFromText('LINESTRING(586782 4504202,586864 4504216)', 26918), 50)
            """
        Then validate that "1186" is in the stored rows
   

    # Spatial joins

    Scenario: What neighborhood is the 'Broad St' station in
         When execute following sql in db "opengeo" and store result in the context
            """
            SELECT subways.name AS subway_name, neighborhoods.name AS neighborhood_name, neighborhoods.boroname AS borough
            FROM   nyc_neighborhoods AS neighborhoods
            JOIN   nyc_subway_stations AS subways ON ST_Contains(neighborhoods.geom, subways.geom)
            WHERE  subways.name = 'Broad St'
            """
        Then validate that "Broad St|Financial District|Manhattan" "string|string|string" seperated by "|" is in the stored rows 

    Scenario: What is the population and racial make-up of the neighborhoods of Manhattan
        When execute following sql in db "opengeo" and store result in the context
            """
            SELECT neighborhoods.name AS neighborhood_name, Sum(census.popn_total) AS population, 
                   Round(100.0 * Sum(census.popn_white) / Sum(census.popn_total),1) AS white_pct,
                   Round(100.0 * Sum(census.popn_black) / Sum(census.popn_total),1) AS black_pct
            FROM   nyc_neighborhoods AS neighborhoods
            JOIN   nyc_census_blocks AS census ON ST_Intersects(neighborhoods.geom, census.geom)
            WHERE  neighborhoods.boroname = 'Manhattan'
            GROUP  BY neighborhoods.name ORDER BY white_pct DESC
            """
        Then validate that following rows are in the stored rows
            | neighborhood_name   | population | white_pct | black_pct |
            | Carnegie Hill       |      19909 |      91.6 |       1.5 |
            | North Sutton Area   |      21413 |      90.3 |       1.2 |
            | West Village        |      27141 |      88.1 |       2.7 |
            | Upper East Side     |     201301 |      87.8 |       2.5 |

    Scenario: What is the total population and total racial made-up in nyc
        When execute following sql in db "opengeo" and store result in the context
            """
            SELECT Round(100.0 * Sum(popn_white) / Sum(popn_total), 1) AS white_pct,
                   Round(100.0 * Sum(popn_black) / Sum(popn_total), 1) AS black_pct, 
                   Sum(popn_total) AS popn_total
            FROM nyc_census_blocks
            """
        Then validate that following rows are in the stored rows
            | white_pct | black_pct | popn_total |
            |      44.7 |      26.6 |    8008278 |

    Scenario: What is racial make-up along the A-train
        When execute following sql in db "opengeo" and store result in the context
            """
            SELECT Round(100.0 * Sum(popn_white) / Sum(popn_total), 1) AS white_pct,
                   Round(100.0 * Sum(popn_black) / Sum(popn_total), 1) AS black_pct, 
                   Sum(popn_total) AS popn_total
            FROM   nyc_census_blocks AS census
            JOIN   nyc_subway_stations AS subways ON ST_DWithin(census.geom, subways.geom, 200)
            WHERE  strpos(subways.routes,'A') > 0
            """
        Then validate that following rows are in the stored rows
            | white_pct | black_pct | popn_total |
            |      42.1 |      23.1 |     185259 |

    Scenario: What subway station is in 'Little Italy', What subway route is it on
        When execute following sql in db "opengeo" and store result in the context
            """
                SELECT s.name, s.routes FROM nyc_subway_stations AS s
                JOIN nyc_neighborhoods AS n ON ST_Contains(n.geom, s.geom)
                WHERE n.name = 'Little Italy'
            """
        Then validate that following rows are in the stored rows
            |   name    | routes |
            | Spring St | 6      |

     Scenario: What are all the neighborhoods served by the 6-train
        When execute following sql in db "opengeo" and store result in the context
            """
            SELECT DISTINCT n.name, n.boroname FROM nyc_subway_stations AS s
            JOIN nyc_neighborhoods AS n ON ST_Contains(n.geom, s.geom)
            WHERE strpos(s.routes,'6') > 0
            """
        Then validate that following rows are in the stored rows
            |        name        | boroname  | 
            | Chinatown          | Manhattan |
            | East Harlem        | Manhattan |
            | Financial District | Manhattan |
            | Gramercy           | Manhattan |
            | Greenwich Village  | Manhattan |
            | Hunts Point        | The Bronx |
            | Little Italy       | Manhattan |

      Scenario: How many people had to be evacuated after 9/11 as 'Battery Park' was off limits for several days
        When execute following sql in db "opengeo" and store result in the context
            """
            SELECT Sum(popn_total) FROM nyc_neighborhoods AS n 
            JOIN nyc_census_blocks AS c ON ST_Intersects(n.geom, c.geom)
            WHERE n.name = 'Battery Park'
            """
        Then validate that following rows are in the stored rows
            | sum  |
            | 9928 |

      # Spatial index

      Scenario: What is the population of 'West Village' using bbox
        When execute following sql in db "opengeo" and store result in the context
            """
            SELECT Sum(popn_total) FROM nyc_neighborhoods neighborhoods 
            JOIN nyc_census_blocks blocks ON neighborhoods.geom && blocks.geom
            WHERE neighborhoods.name = 'West Village'
            """
        Then validate that following rows are in the stored rows
            | sum   |
            | 50325 |

      Scenario: What is the population of 'West Village' using ST_Intersects
        When execute following sql in db "opengeo" and store result in the context
            """
            SELECT Sum(popn_total) FROM nyc_neighborhoods neighborhoods 
            JOIN nyc_census_blocks blocks ON ST_Intersects(neighborhoods.geom, blocks.geom)
            WHERE neighborhoods.name = 'West Village'
            """
        Then validate that following rows are in the stored rows
            | sum   |
            | 27141 |


               
    # Projecting/Transforming
    Scenario: Convert the coordinates of the ‘Broad St’ subway station into geographics
        When execute following sql in db "opengeo" and store result in the context
            """
            SELECT ST_AsText(ST_Transform(geom,4326)) FROM nyc_subway_stations WHERE name = 'Broad St';
            """
        Then validate that following rows are in the stored rows
            |                  st_astext                |
            | POINT(-74.0106714688735 40.7071048155841) |

    Scenario: What is the length of all streets in New York, as measured in UTM 18
        When execute following sql in db "opengeo" and store result in the context
            """
            SELECT Sum(ST_Length(geom))::integer FROM nyc_streets
            """
        Then validate that following rows are in the stored rows
            |    sum   | 
            | 10418905 |

    Scenario: What is the length of all streets in New York, as measured in SRID 2831
        When execute following sql in db "opengeo" and store result in the context
            """
            SELECT Sum(ST_Length(ST_Transform(geom,2831)))::integer FROM nyc_streets
            """
        Then validate that following rows are in the stored rows
            |    sum    |
            | 10421994  |

    Scenario: What is the KML representation of the point at ‘Broad St’ subway station
        When execute following sql in db "opengeo" and store result in the context
            """
            SELECT ST_AsKML(geom) FROM nyc_subway_stations WHERE name = 'Broad St'
            """
        Then validate that following rows are in the stored rows
            |                                      st_askml                                    | 
            | <Point><coordinates>-74.010671468873468,40.707104815584088</coordinates></Point> |


    # Geography
    Scenario: What is the distance between Los Angeles and Paris
        When execute following sql in db "opengeo" and store result in the context
            """
            SELECT Round(ST_Distance(
                    ST_GeometryFromText('POINT(-118.4079 33.9434)', 4326), -- Los Angeles (LAX)
                    ST_GeometryFromText('POINT(2.5559 49.0083)', 4326)     -- Paris (CDG)
                  )::numeric, 1)
            """
        Then validate that following rows are in the stored rows
            | round |
            | 121.9 |
        When execute following sql in db "opengeo" and store result in the context
            """
            SELECT Round(ST_Distance(
                    ST_GeographyFromText('POINT(-118.4079 33.9434)'), -- Los Angeles (LAX)
                    ST_GeographyFromText('POINT(2.5559 49.0083)')     -- Paris (CDG)
                  )::numeric, 1)
            """
        Then validate that following rows are in the stored rows
            | round     |
            | 9124665.3 |


    Scenario: Geography type basic test
        When below sql is executed in "opengeo" db 
            """
            CREATE TABLE airports (code VARCHAR(3), geog GEOGRAPHY)
            """
        And  below sql is executed in "opengeo" db
            """
            INSERT INTO airports VALUES
                ('LAX', 'POINT(-118.4079 33.9434)'),
                ('CDG', 'POINT(2.5559 49.0083)'),
                ('REK', 'POINT(-21.8628 64.1286)')
            """
        And execute following sql in db "opengeo" and store result in the context
            """
            select f_table_name, f_geography_column, srid, type from geography_columns
            """
        Then validate that following rows are in the stored rows
            | f_table_name | f_geography_column | srid |   type   |
            | airports     | geog               |    0 | Geometry |
        When execute following sql in db "opengeo" and store result in the context
            """
            SELECT code, ST_X(geog::geometry) AS longitude FROM airports
            """
        Then validate that following rows are in the stored rows
            | code | longitude |
            | LAX  | -118.4079 |
            | CDG  |    2.5559 |
            | REK  |  -21.8628 |
        When execute following sql in db "opengeo" and store result in the context
            """
            SELECT code, Round(ST_Distance(geog, 'POINT(-118.4079 33.9434)'))::integer AS distance FROM airports
            """
        Then validate that following rows are in the stored rows
             | code | distance  |
             | LAX  |        0  |
             | CDG  |  9124665  |
             | REK  |  6969661  |
        When execute following sql in db "opengeo" and store result in the context
            """
            SELECT code, ST_DWithin(geog, 'POINT(-118.4079 33.9434)', 100) AS within FROM airports;
            """
        Then validate that following rows are in the stored rows
            | code | within |
            | CDG  | False  |
            | REK  | False  |
            | LAX  | True   |
        When execute following sql in db "opengeo" and store result in the context
            """
            SELECT substr(ST_AsText(ST_Buffer(geog, 100)), 0, 20) AS buffer_snipet FROM airports
            """
        Then validate that following rows are in the stored rows
            |     buffer_snipet    |
            |  POLYGON((2.55726738 |
            |  POLYGON((-21.860746 |
            |  POLYGON((-118.40681 |

    # Geometry constructing functions
    Scenario: How many points for liberty island with 500m and -50m buffer zone
        When below sql is executed in "opengeo" db
            """
            CREATE TABLE liberty_island_zone AS SELECT ST_Buffer(geom,500) AS geom
            FROM nyc_census_blocks WHERE blkid = '360610001009000'
            """
        And execute following sql in db "opengeo" and store result in the context
            """
            select st_npoints(geom) from liberty_island_zone
            """
        Then validate that following rows are in the stored rows
            | st_npoints |
            |         43 |
        When below sql is executed in "opengeo" db
            """
            CREATE TABLE liberty_island_zone2 AS SELECT ST_Buffer(geom, -50) AS geom
            FROM nyc_census_blocks WHERE blkid = '360610001009000' 
            """
        And execute following sql in db "opengeo" and store result in the context
            """
            select st_npoints(geom) from liberty_island_zone2
            """
        Then validate that following rows are in the stored rows
            | st_npoints |
            |         11 | 
    
    Scenario: How many points for intersection of two buffer
        When execute following sql in db "opengeo" and store result in the context
            """
            SELECT ST_NPoints(ST_AsText(ST_Intersection(ST_Buffer('POINT(0 0)', 2), ST_Buffer('POINT(3 0)', 2))))
            """
        Then validate that following rows are in the stored rows
            | st_npoints |
            |         17 | 
 
    Scenario: How many points for union of two buffer
        When execute following sql in db "opengeo" and store result in the context
            """
            SELECT ST_NPoints(ST_AsText(ST_Union(ST_Buffer('POINT(0 0)', 2), ST_Buffer('POINT(3 0)', 2))))
            """
        Then validate that following rows are in the stored rows
            | st_npoints |
            |         53 | 
        
    Scenario: merging census blocks using union
        When below sql is executed in "opengeo" db
            """
            CREATE TABLE nyc_census_counties AS
            SELECT ST_Union(geom) AS geom, SubStr(blkid,1,5) AS countyid FROM nyc_census_blocks GROUP BY countyid
            """
        And execute following sql in db "opengeo" and store result in the context
            """
            SELECT SubStr(blkid,1,5) AS countyid, Round(Sum(ST_Area(geom)))::integer AS area
            FROM nyc_census_blocks
            GROUP BY countyid ORDER BY countyid
            """
        Then validate that following rows are in the stored rows
             | countyid |   area    |
             | 36005    | 109807440 |
             | 36047    | 184906576 |
             | 36061    |  58973522 |
             | 36081    | 283764734 |
             | 36085    | 149806078 |
        When execute following sql in db "opengeo" and store result in the context
            """
            SELECT countyid, Round(ST_Area(geom))::integer AS area
            FROM nyc_census_counties ORDER BY countyid
            """
        Then validate that following rows are in the stored rows
             | countyid |   area    |
             | 36005    | 109807440 |
             | 36047    | 184906576 |
             | 36061    |  58973522 |
             | 36081    | 283764734 |
             | 36085    | 149806078 |

    # More spatial joins
    Scenario: nyc census data with tract geometry: List top 10 New York neighborhoods ordered by the proportion of people who have graduate degrees
        When below sql is executed in "opengeo" db
            """
            CREATE TABLE nyc_census_tract_geoms AS
            SELECT ST_Union(geom) AS geom, SubStr(blkid,1,11) AS tractid FROM nyc_census_blocks GROUP BY tractid
            """
        And  below sql is executed in "opengeo" db
            """
            CREATE TABLE nyc_census_tracts AS
            SELECT g.geom, a.* FROM nyc_census_tract_geoms g JOIN nyc_census_sociodata a ON g.tractid = a.tractid;
            """
        And  execute following sql in db "opengeo" and store result in the context
            """
            SELECT Round(100.0 * Sum(t.edu_graduate_dipl) / Sum(t.edu_total), 1) AS graduate_pct, n.name, n.boroname
            FROM nyc_neighborhoods n JOIN nyc_census_tracts t ON ST_Intersects(n.geom, t.geom) 
            WHERE t.edu_total > 0 GROUP BY n.name, n.boroname ORDER BY graduate_pct DESC LIMIT 10;
            """
        Then validate that following rows are in the stored rows
             | graduate_pct |       name        | boroname  |
             |         40.4 | Carnegie Hill     | Manhattan |
             |         40.2 | Flatbush          | Brooklyn  |
             |         34.8 | Battery Park      | Manhattan |
             |         33.9 | North Sutton Area | Manhattan |


    Scenario: avoiding double count issues for ST_Intersects
        When execute following sql in db "opengeo" and store result in the context
            """
            SELECT Round(100.0 * Sum(t.edu_graduate_dipl) / Sum(t.edu_total), 1) AS graduate_pct, n.name, n.boroname
            FROM nyc_neighborhoods n JOIN nyc_census_tracts t ON ST_Contains(n.geom, ST_Centroid(t.geom))
            WHERE t.edu_total > 0 GROUP BY n.name, n.boroname ORDER BY graduate_pct DESC LIMIT 10;
            """
        Then validate that following rows are in the stored rows
            | graduate_pct |       name        | boroname   |
            |         49.2 | Carnegie Hill     | Manhattan  |
            |         39.5 | Battery Park      | Manhattan  |
            |         34.3 | Upper East Side   | Manhattan  |
            |         33.6 | Upper West Side   | Manhattan  |
            |         32.5 | Greenwich Village | Manhattan  |
            |         32.2 | Tribeca           | Manhattan  |
            |         31.3 | North Sutton Area | Manhattan  |
            |         30.8 | West Village      | Manhattan  |
            |         30.1 | Downtown          | Brooklyn   |
            |         28.4 | Cobble Hill       | Brooklyn   |

    # 3D
    Scenario: 3D distance
        When execute following sql in db "opengeo" and store result in the context
            """
            SELECT Round(ST_3DDistance(
              'POLYHEDRALSURFACE Z (
                  ((0 0 0, 0 1 0, 1 1 0, 1 0 0, 0 0 0)),
                  ((0 0 0, 0 1 0, 0 1 1, 0 0 1, 0 0 0)),
                  ((0 0 0, 1 0 0, 1 0 1, 0 0 1, 0 0 0)),
                  ((1 1 1, 1 0 1, 0 0 1, 0 1 1, 1 1 1)),
                  ((1 1 1, 1 0 1, 1 0 0, 1 1 0, 1 1 1)),
                  ((1 1 1, 1 1 0, 0 1 0, 0 1 1, 1 1 1))
               )'::geometry,
               'POINT Z (2 2 2)'::geometry
            )::numeric, 5)
            """
        Then validate that following rows are in the stored rows
            | st_3ddistance    |
            | 1.73205 |

        When execute following sql in db "opengeo" and store result in the context
            """
            SELECT Round(ST_3DDistance( 'POINT Z (1 1 1)'::geometry, 'POINT Z (2 2 2)'::geometry)::numeric, 5) 
            """
        Then validate that following rows are in the stored rows
            | st_3ddistance    |
            | 1.73205 |

    Scenario: N-D index operator
        When execute following sql in db "opengeo" and store result in the context
            """
            SELECT gid, name FROM nyc_streets
            WHERE geom &&& ST_SetSRID('LINESTRING(586785 4492901,587561 4493037)',26918)
            """
        Then validate that following rows are in the stored rows
            | gid  |       name     |
            |    1 | Shore Pky S    |
            |   57 | W 5th St       |
            |   69 | Colby Ct       |


    # Index-based KNN (K nearest neighbours)
    Scenario: Closest streets to Broad Street station using KNN
        When execute following sql in db "opengeo" and store result in the context
            """
            WITH closest_candidates AS (
              SELECT
                streets.gid,
                streets.name,
                streets.geom
              FROM
                nyc_streets streets
              ORDER BY
                streets.geom <->
                'SRID=26918;POINT(583571.905921312 4506714.34119218)'::geometry
              LIMIT 100
            )
            SELECT gid, name
            FROM closest_candidates
            ORDER BY
              ST_Distance(
                geom,
                'SRID=26918;POINT(583571.905921312 4506714.34119218)'::geometry
                )
            LIMIT 1;
            """
        Then validate that following rows are in the stored rows
            | gid   |  name   |
            | 17385 | Wall St |

    Scenario: 10 nearest stations to Broad St station
        When execute following sql in db "opengeo" and store result in the context
            """
            SELECT gid, name
            FROM nyc_subway_stations
            ORDER BY geom <-> 'SRID=26918;POINT(583571.905921312
            4506714.34119218)'::geometry
            LIMIT 10;
            """
        Then validate that following rows are in the stored rows
            | gid | name        |
            | 332 | Broad St    |
            | 373 | Wall St     |
            | 374 | Wall St     |
            | 366 | Rector St   |
            |   2 | Rector St   |
            |   1 | Cortlandt St|

    Scenario: ST_AsGML should work with in-memory lookup of spatial_ref_sys
        When execute following sql in db "opengeo" and store result in the context
            """
            SELECT substr(ST_AsX3D(geom), 1400, 100) FROM nyc_census_blocks
            WHERE blkid = '360050001009000';
            """
        Then validate that following rows are in the stored rows
            | substr                                                                                               |
            | 370 371 372 373 374 375 376 377'>595206.589571490767412 4516105.184799655340612 595214.8743579722940 |
        When execute following sql in db "opengeo" and store result in the context
            """
            SELECT substr(ST_AsGML(geom), 0, 200) FROM nyc_census_blocks WHERE blkid = '360050001009000'
            """
        Then validate that following rows are in the stored rows
            | substr                                                                                                | 
            | <gml:MultiPolygon srsName="EPSG:26918"><gml:polygonMember><gml:Polygon><gml:outerBoundaryIs><gml:LinearRing><gml:coordinates>595206.589571490767412,4516105.184799655340612 595214.874357972294092,4516 |
        When execute following sql in db "opengeo" and store result in the context
            """
            SELECT ST_AsGML(geog) FROM airports;
            """
        Then validate that following rows are in the stored rows
            |                                                        st_asgml                                           |       
            | <gml:Point srsName="EPSG:4326"><gml:coordinates>2.5559,49.008299999999998</gml:coordinates></gml:Point>   |
            | <gml:Point srsName="EPSG:4326"><gml:coordinates>-21.8628,64.128600000000006</gml:coordinates></gml:Point> |
            | <gml:Point srsName="EPSG:4326"><gml:coordinates>-118.407899999999998,33.943399999999997</gml:coordinates></gml:Point> |


    Scenario: Import/Export using different format
        When execute following sql in db "opengeo" and store result in the context
            """
            SELECT ST_AsText(ST_GeomFromGML('<gml:LineString srsName="EPSG:4269">
                <gml:coordinates>-71.16028,42.258729 -71.160837,42.259112 -71.161143,42.25932</gml:coordinates>
                </gml:LineString>'))
            """
        Then validate that following rows are in the stored rows
            |                                 st_astext                                | 
            | LINESTRING(-71.16028 42.258729,-71.160837 42.259112,-71.161143 42.25932) |
        When execute following sql in db "opengeo" and store result in the context
            """
            SELECT ST_AsGML(3, ST_GeomFromText('POINT(5.234234233242 6.34534534534)',4326), 5, 17)
            """
        Then validate that following rows are in the stored rows
            | st_asgml |
            | <gml:Point srsName="urn:ogc:def:crs:EPSG::4326"><gml:pos srsDimension="2">6.34535 5.23423</gml:pos></gml:Point> |
        When execute following sql in db "opengeo" and store result in the context
            """
            SELECT substr(ST_AsGeoJSON(geom), 0, 200) FROM nyc_census_blocks WHERE blkid = '360050001009000'
            """
        Then validate that following rows are in the stored rows
            | substr                                                                                                 | 
            | {"type":"MultiPolygon","coordinates":[[[[595206.589571491,4516105.18479966],[595214.874357972,4516094.08370369],[595221.417038022,4516085.47920627],[595222.952499512,4516083.21821789],[595224.5418065 |

    Scenario: Linear approximation of closed curve polygon
        When below sql is executed in "opengeo" db
            """
            CREATE TABLE t_postgis_linear(name text, geom geometry)
            """
        And below sql is executed in "opengeo" db
            """
            INSERT INTO t_postgis_linear VALUES
                ('curve_polygon',
                 'CURVEPOLYGON(CIRCULARSTRING(-192.63 96.09,-236.72 -84.3,-262.33
                  -256.89,-267.71 -409.94,-252.5 -533,-217.73 -617.69,-165.78
                  -658.24,-100.19 -651.89, -25.42 -599.06,53.44 -503.37,130.99
                  -371.32,201.97 -211.92,261.53 -36.04,305.62 144.35,331.23
                  316.94,336.61 469.99,321.4 593.05,286.64 677.74,234.69 718.29,169.09
                  711.94,94.32 659.11,15.47 563.42,-62.09 431.37,-133.07
                  271.98,-192.63 96.09))')
            """
        When execute sql "SELECT st_isclosed(geom) as closed from t_postgis_linear where name='curve_polygon'" in db "opengeo" and store result in the context
        Then validate that following rows are in the stored rows
            | closed |
            | True   |
        When execute sql "SELECT st_isclosed(st_curvetoline(geom)) as closed from t_postgis_linear where name='curve_polygon'" in db "opengeo" and store result in the context
        Then validate that following rows are in the stored rows
            | closed |
            | True   |

    # TODO:
    #
    # 1) JSON-C: SELECT
    # ST_AsText(ST_GeomFromGeoJSON('{"type":"Point","coordinates":[-48.23456,20.12345]}')) As wkt;
