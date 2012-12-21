This is where unit tests (not written in Java), that test components.  Again, these tests want to live here because they need to build from a subset of the source and don't lend themselves to testing via the libcdbtest directory approach.

Tests in here exercise functionality that doesn't require being run in a postgres server.  
