#include <new>
#include <exception>
#include <stdexcept>

#include <iostream>
#include <fstream>
#include <iomanip>
#include <sstream>

#include <iterator>
#include <list>
#include <vector>
#include <map>
#include <string>

#include <functional>
#include <algorithm>

#ifndef _WIN32
#include <getopt.h>
#else
#include "getopt.h"
#endif

// Get Chuck's really ugly but really simple and lightweight wrapper around libpq
#include "libpq_wrapper.h"

using namespace libpq_wrapper;

size_t seg = 0;
size_t reference_host = 0;
string segname;
string mastername;


// For each catalog table we want to check, make a class that describes the row from that table.
// Also, make a (not) equal test that compares the essential columns.  
// Don't include columns that are OK to be different on different segDBs.

class pg_namespace_row
{
public:
	Oid oid;
	string nspname;
	Oid nspowner;
	// nspacl

	bool operator==(const pg_namespace_row & pgn) const
	{
		if (oid != pgn.oid)
			return false;
		if (nspname != pgn.nspname)
			return false;
		if (nspowner != pgn.nspowner)
			return false;
		return true;
	}
	bool operator!=(const pg_namespace_row & pgn) const
	{
		return !operator==(pgn);
	}
	bool operator<(const pg_namespace_row & pgn) const
	{
		if (oid < pgn.oid)
			return true;
		if (oid > pgn.oid)
			return false;
		if (nspname < pgn.nspname)
			return true;

		return false;
	}
	bool operator>(const pg_namespace_row & pgn) const
		{
			if (oid > pgn.oid)
				return true;
			if (oid < pgn.oid)
				return false;
			if (nspname > pgn.nspname)
				return true;

			return false;
		}
	friend ostream & operator<< (ostream & os, pg_namespace_row pgn) 
	{
		os << pgn.oid << " " << pgn.nspname;
		return os;
	}
};


class pg_class_row
{
public:
	Oid oid;
	string relname;
	Oid relnamespace;
	Oid reltype;
	Oid relowner;
	Oid relam;
	Oid relfilenode;
	Oid reltablespace;
	int relpages;
	double reltuples;
	Oid reltoastrelid;
	Oid reltoastidxid;
	Oid relaosegrelid;
	Oid relaosegidxid;
	bool relhasindex;
	bool relisshared;
	char relkind;
	short relnatts;
	short relchecks;
	short reltriggers;
	short relukeys;
	short relfkeys;
	short relrefs;
	bool relhasoids;
	bool relhaspkey;
	bool relhasrules;
	bool relhassubclass;
	// relfrozenxid
	// relacl
	// reloptions

	bool operator==(const pg_class_row & pgc) const
	{
		if (oid != pgc.oid)
			return false;
		if (relnamespace != pgc.relnamespace)
			return false;
		if (relname != pgc.relname)
			return false;
		if (relkind != pgc.relkind)
			return false;
		if (relnatts != pgc.relnatts)
			return false;
		return true;
	}
	bool operator!=(const pg_class_row & pgn) const
	{
		return !operator==(pgn);
	}
	bool operator<(const pg_class_row & pgc) const
	{
		if (oid < pgc.oid)
			return true;
		if (oid > pgc.oid)
			return false;
		if (relname < pgc.relname)
			return true;

		return false;
	}
	bool operator>(const pg_class_row & pgc) const
	{
		if (oid > pgc.oid)
			return true;
		if (oid < pgc.oid)
			return false;
		if (relname > pgc.relname)
			return true;

		return false;
	}
	friend ostream & operator<< (ostream & os, pg_class_row pgc) 
	{
		os << pgc.oid << " " << pgc.relname << " " << pgc.relkind;
		return os;
	}
};


class pg_attribute_row
{
public:
	Oid attrelid;
	string attname;
	Oid atttypid;
	int attstattarget;
	short attlen;
	short attnum;
	int attndims;
	int attcacheoff;
	int atttypmod;
	bool attbyval;
	char attstorage;
	char attalign;
	bool attnotnull;
	bool atthasdef;
	bool attisdropped;
	bool attislocal;
	int attinhcount;

	bool operator==(const pg_attribute_row & pga) const
	{
		if (attrelid != pga.attrelid)
			return false;
		if (attnum != pga.attnum)
			return false;
		if (attname != pga.attname)
			return false;
		if (atttypid != pga.atttypid)
			return false;
		return true;
	}
	bool operator!=(const pg_attribute_row & pga) const
	{
		return !operator==(pga);
	}
	friend ostream & operator<< (ostream & os, pg_attribute_row pga) 
	{
		os <<  pga.attrelid << "," << pga.attnum << ": " << pga.attname << " "<<  pga.atttypid;
		return os;
	}
};

class pg_attribute_key
{
public:
	Oid attrelid;
	short attnum;
	pg_attribute_key(Oid r, short n): attrelid(r),attnum(n) { }
	bool operator==(const pg_attribute_key & k) const
	{
		return attrelid == k.attrelid && attnum == k.attnum;
	}
	bool operator!=(const pg_attribute_key & k) const
	{
		return !operator==(k);
	}
	bool operator>(const pg_attribute_key & k) const
	{
		if (attrelid > k.attrelid)
			return true;
		if (attrelid == k.attrelid && attnum > k.attnum)
			return true;
		return false;
	} 
	bool operator<(const pg_attribute_key & k) const
	{
		if (attrelid < k.attrelid)
			return true;
		if (attrelid == k.attrelid && attnum< k.attnum)
			return true;
		return false;
	} 
	 
};


class pg_type_row
{
public:
	Oid oid;
	string typname;
	Oid typnamespace;
	Oid typowner;
	short typlen;
	bool typbyval;
	char typtype;
	bool typisdefined;
	char typdelim;
	Oid typrelid;
	Oid typelem;
	//Oid typarray;  pg 8.3 or later

	bool operator==(const pg_type_row & pgt) const
	{
		if (oid != pgt.oid)
			return false;
		if (typname != pgt.typname)
			return false;
		if (typnamespace != pgt.typnamespace)
			return false;
		if (typlen != pgt.typlen)
			return false;
		return true;
	}
	bool operator!=(const pg_type_row & pgt) const
	{
		return !operator==(pgt);
	}
	friend ostream & operator<< (ostream & os, pg_type_row pgt) 
	{
		os << pgt.oid << " " << pgt.typname;
		return os;
	}

};

class pg_exttable_row
{
public:
	Oid reloid;
	string location;
	char fmtype;
	string fmopts;
	string command;
	int rejectlimit;
	char rejectlimittype;
	Oid fmterrtbl;
	int encoding;

	bool operator==(const pg_exttable_row & pgt) const
	{
		if (reloid != pgt.reloid)
			return false;
		if (location != pgt.location)
			return false;
		if (fmtype != pgt.fmtype)
			return false;
		if (command != pgt.command)
			return false;
		return true;
	}
	bool operator!=(const pg_exttable_row & pgt) const
	{
		return !operator==(pgt);
	}
	friend ostream & operator<< (ostream & os, pg_exttable_row pgt) 
	{
		os << pgt.reloid << " " << pgt.location << " " << pgt.command;
		return os;
	}

};


//  associative containers to hold the catalog rows
 
typedef map<Oid,pg_namespace_row> pg_namespace_map_by_oid;
typedef map<std::string,pg_namespace_row> pg_namespace_map_by_name;
typedef map<Oid,pg_class_row> pg_class_map;
typedef map<const pg_attribute_key,pg_attribute_row> pg_attribute_map;
typedef map<Oid,pg_type_row> pg_type_map;


// Describe the configuration of the server

class gp_config_info
{
public:
	int content;
	bool definedprimary;
	int dbid;
	bool isprimary;
	bool valid;
	std::string hostname;
	std::string port;

	
	//bool is_corrupt;  // we set this if we think it is corrupt.
	
	dbconnection conn;

	pg_namespace_map_by_name pg_namespace_by_name;
	pg_namespace_map_by_oid pg_namespace_by_oid;
	pg_class_map pg_class;
	pg_attribute_map pg_attribute;
	pg_type_map pg_type;

	string segname() const
	{
		ostringstream temp;
		temp << "Host " << hostname;
		temp << " Seg" << content;
		if (content==-1 && isprimary)
			temp << " (master)";
		else if (isprimary)
			temp << " (primary)";
		else
			temp << " (mirror)";
		temp << " ";

		return temp.str();
	}
};

pg_namespace_map_by_name * reference_namespace_map_by_name;
pg_namespace_map_by_oid * reference_namespace_map_by_oid;
pg_class_map * reference_class_map;
pg_attribute_map * reference_attribute_map;
pg_type_map * reference_type_map;

pg_class_map * current_class_map;

bool corrupted = false;

void print_pg_namespace(pair<const std::string,pg_namespace_row> & p)
{
	cout << segname << " "<< p.second.oid << " " << p.second.nspname << endl;
}


struct verify_pg_namespace_against_reference: public binary_function< pg_namespace_map_by_name *, pair<const std::string, pg_namespace_row>, void > 
{
void operator()(pg_namespace_map_by_name * ref, pair<const std::string, pg_namespace_row> & p) const
{
	pg_namespace_map_by_name::const_iterator r = ref->find(p.first);
	if (r == ref->end())
	{
		cout <<  segname << " pg_namespace has extra row:  Oid " << p.second << endl;
		corrupted = true;
	}
	else if (r->second != p.second)
		// temp namespaces (schemas) have different oids on master from segdbs.
		if (r->second.nspname.substr(0,8) != "pg_temp_")
		{
			cout <<  segname << " pg_namespace has row not matched:   Oid " << p.second << " <-> " << r->second << endl;
			corrupted = true;
		}
}
};

void print_pg_class(pair<const Oid,pg_class_row> & p)
{
	cout << p.second << endl;
}
 
struct verify_pg_class_to_pg_namespace: public unary_function< const pair<const Oid, const pg_class_row>&, void > 
{
void operator()(const pair<const Oid,const pg_class_row> & p) const
{
	pg_namespace_map_by_oid::const_iterator r =
			reference_namespace_map_by_oid->find(p.second.relnamespace);
	if (r == reference_namespace_map_by_oid->end())
	{
		cout <<  segname << " pg_class row points to non-existant namespace:   Oid " << p.second << endl;
		corrupted = true;
	}

}
};


struct verify_pg_class_against_reference: public binary_function<pg_class_map *, pair<const Oid,pg_class_row>, void > 
{
void operator()(pg_class_map * ref, pair<const Oid,pg_class_row> & p) const
{
	pg_class_map::const_iterator r = ref->find(p.first);
	if (r == ref->end())
	{
		// Hack workaround to bug in CREATE EXTERNAL TABLE.  It was creating TOAST tables for ext tables, but only on the segDBs, not the master.
		if (p.second.relname.substr(0,9) != "pg_toast_")
		{
			cout <<  segname << " pg_class has extra row:  Oid " << p.second << endl;
			corrupted = true;
		}
	}
	else if (r->second != p.second)
	{
		cout <<  segname << " pg_class row does not match:   Oid " << p.second << " <-> " << r->second << endl;
		corrupted = true;
	}
}
};

struct verify_reference_against_pg_class: public binary_function<pg_class_map *, pair<const Oid,pg_class_row>, void > 
{
void operator()(pg_class_map * ref, pair<const Oid,pg_class_row> & p) const
{
	pg_class_map::const_iterator r = ref->find(p.first);
	if (r == ref->end())
	{
		// Hack workaround to bug in CREATE EXTERNAL TABLE.  It was creating TOAST tables for ext tables, but only on the segDBs, not the master.
		if (p.second.relname.substr(0,9) != "pg_toast_")
		{
			cout <<  segname << " pg_class is missing row:  Oid " << p.second << endl;
			corrupted = true;
	
		}
	}
}
};

struct verify_pg_class_to_pg_attribute: public unary_function< pair<const Oid,pg_class_row>&, void > 
{
void operator()(pair<const Oid,pg_class_row> & p) const
{
	if (p.second.relnatts > 0)
	{
		pg_attribute_key k(p.first, 1);
		pg_attribute_map::const_iterator r = 	reference_attribute_map->find(k);
		if (r == reference_attribute_map->end())
		{
			cout <<  segname << " pg_class row is missing it's attributes:   Oid " << p.second << endl;
			corrupted = true;
		}
	}
}
};

struct verify_pg_attribute_to_pg_class: public unary_function< pair<const pg_attribute_key,pg_attribute_row>&, void > 
{
void operator()(pair<const pg_attribute_key,pg_attribute_row> & p) const
{
	pg_class_map::const_iterator r = reference_class_map->find(p.first.attrelid);
	if (r == reference_class_map->end())
	{
		cout << segname << " pg_attribute row points to non-existant pg_class row:   attrelid " << p.second << endl;
		corrupted = true;
	}

}
};

struct verify_pg_attribute_against_reference: public binary_function< pg_attribute_map *, pair<const pg_attribute_key,pg_attribute_row>, void > 
{
void operator()(pg_attribute_map * ref, pair<const pg_attribute_key,pg_attribute_row> & p) const
{
	pg_attribute_map::const_iterator r = ref->find(p.first);
	if (r == ref->end())
	{
		// Hack workaround to bug in CREATE EXTERNAL TABLE.  It was creating TOAST tables for ext tables, but only on the segDBs, not the master.
		pg_class_map::const_iterator r2= current_class_map->find(p.first.attrelid);
		if (r2 == current_class_map->end() || r2->second.relname.substr(0,9) != "pg_toast_")
		{
		cout <<  segname << " pg_attribute has extra row:   attrelid " << p.second << endl;
		corrupted = true;
		}
	}
	else if (r->second != p.second)
	{
		cout <<  segname << " pg_attribute has row that doesn't match:  attrelid " << p.second << " <-> " << r->second << endl;
		corrupted = true;
	}
}
};

struct verify_reference_against_pg_attribute: public binary_function< pg_attribute_map *, pair<const pg_attribute_key,pg_attribute_row>, void > 
{
void operator()(pg_attribute_map * ref, pair<const pg_attribute_key,pg_attribute_row> & p) const
{
	pg_attribute_map::const_iterator r = ref->find(p.first);
	if (r == ref->end())
	{
		cout <<  segname << " pg_attribute is missing row:   attrelid " << p.second << endl;
	}
}
};

struct verify_pg_type_to_pg_class: public unary_function< pair<const Oid,pg_type_row>&, void > 
{
void operator()(pair<const Oid,pg_type_row> & p) const
{
	if (p.second.typrelid != 0)
	{
		pg_class_map::const_iterator r =
				reference_class_map->find(p.second.typrelid);
		if (r == reference_class_map->end())
		{
			cout << segname << " pg_type has row that points to non-existant pg_class row:   Oid " << p.second << endl;
			corrupted = true;
		}
	}
}
};

struct verify_pg_type_against_reference: public binary_function< pg_type_map *, pair<const Oid,pg_type_row>, void > 
{
void operator()(pg_type_map * ref, pair<const Oid,pg_type_row> & p) const
{
	pg_type_map::const_iterator r = ref->find(p.first);
	if (r == ref->end())
	{
		// Hack workaround to bug in CREATE EXTERNAL TABLE.  It was creating TOAST tables for ext tables, but only on the segDBs, not the master.
		if (p.second.typname.substr(0,9) != "pg_toast_")
		{
			cout <<  segname << " pg_type has extra row:   oid " << p.second << endl;
			corrupted = true;
		}
	}
	else if (r->second != p.second)
	{
		cout << segname << " pg_type has row that doesn't match:  oid " << p.second  << " <-> " << r->second << endl;
		corrupted = true;
	}
}
};

struct verify_reference_against_pg_type: public binary_function< pg_type_map *, pair<const Oid,pg_type_row>, void > 
{
void operator()(pg_type_map * ref, pair<const Oid,pg_type_row> & p) const
{
	pg_type_map::const_iterator r = ref->find(p.first);
	if (r == ref->end())
	{
		cout <<  segname << " pg_type is missing row row:   oid " << p.second << endl;
		corrupted = true;
	}
}
};




void read_catalog_info(vector<gp_config_info> & config, dbresult::size_type segdbcount, string & conninfo, string & masterversion)
{
	dbresult::size_type seg;
	// Connect to each segdb.
	for (seg = 1; seg < segdbcount; ++seg)
	{
		ostringstream segdbconninfo;
		segdbconninfo << conninfo << " host=" << config[seg].hostname
				<< " port=" << config[seg].port;

		try
		{
			config[seg].conn.connect(segdbconninfo.str(), true);
		}
		catch(connection_exception & ce)
		{
			cerr << "Could not connect to segDB " << segdbconninfo << ":  " << ce.what() << endl;
			vector<gp_config_info>::iterator c =  config.begin() + seg;
			config.erase(c);
			segdbcount--;
			seg--;
			continue;
		}
			

		dbresult r2 = config[seg].conn.exec("select version()");
		if (r2[0][0].as_string() != masterversion)
		{
			cout << "version mismatch on segment " << config[seg].segname()
					<< endl;
			cout << "Master version: "<< masterversion << "\n"
					<< "Segment version: " << r2[0][0].as_string() << endl;
			corrupted = true;

		}

	}

	for (seg = 0; seg < segdbcount; seg++)
	{

		dbresult
				pgnamespaceresult =
						config[seg].conn.exec("select oid,* from pg_catalog.pg_namespace order by oid");

		for (dbresult::const_iterator rp = pgnamespaceresult.begin(); rp 	!=pgnamespaceresult.end(); ++rp)
		{
			pg_namespace_row pgn;

			pgn.oid = rp[0].as_oid();
			pgn.nspname = rp[1].as_string();
			pgn.nspowner = rp[2].as_oid();

			config[seg].pg_namespace_by_oid[pgn.oid] = pgn;
			config[seg].pg_namespace_by_name[pgn.nspname] = pgn;
		}
	}

	for (seg = 0; seg < segdbcount; seg++)
	{

		dbresult
				pgclassresult =
						config[seg].conn.exec("select oid,* from pg_catalog.pg_class order by oid");

		for (dbresult::const_iterator rp = pgclassresult.begin(); rp !=pgclassresult.end(); ++rp)
		{
			pg_class_row pgc;

			pgc.oid = rp[0].as_oid();
			pgc.relname = rp[1].as_string();
			pgc.relnamespace = rp[2].as_oid();
			pgc.reltype = rp[3].as_oid();
			pgc.relowner = rp[4].as_oid();
			pgc.relam = rp[5].as_oid();
			pgc.relfilenode = rp[6].as_oid();
			pgc.reltablespace = rp[7].as_oid();
			pgc.relpages = rp[8].as_int();
			pgc.reltuples = rp[9].as_double();
			pgc.reltoastrelid = rp[10].as_oid();
			pgc.reltoastidxid = rp[11].as_oid();
			pgc.relaosegrelid = rp[12].as_oid();
			pgc.relaosegidxid = rp[13].as_oid();
			pgc.relhasindex = rp[14].as_bool();
			pgc.relisshared = rp[15].as_bool();
			pgc.relkind = rp[16].c_str()[0];
			pgc.relnatts = rp[17].as_int();
			config[seg].pg_class[pgc.oid] = pgc;
		}
	}

	for (seg = 0; seg < segdbcount; seg++)
	{

		dbresult
				pgattributeresult =
						config[seg].conn.exec("select * from pg_catalog.pg_attribute order by attrelid,attnum,attname");

		for (dbresult::const_iterator rp = pgattributeresult.begin(); rp	!=pgattributeresult.end(); ++rp)
		{
			pg_attribute_row pga;

			pga.attrelid = rp[0].as_oid();
			pga.attname = rp[1].as_string();
			pga.atttypid = rp[2].as_oid();
			pga.attstattarget = rp[3].as_int();
			pga.attlen = rp[4].as_int();
			pga.attnum = rp[5].as_int();
			pga.attndims = rp[6].as_int();

			pg_attribute_key k(pga.attrelid,pga.attnum);
			config[seg].pg_attribute[k] = pga;
		}
	}

	for (seg = 0; seg < segdbcount; seg++)
	{

		dbresult
				pgtyperesult =
						config[seg].conn.exec("select oid,* from pg_catalog.pg_type order by oid");

		for (dbresult::const_iterator rp = pgtyperesult.begin(); rp !=pgtyperesult.end(); ++rp)
		{
			pg_type_row pgt;

			pgt.oid = rp[0].as_oid();
			pgt.typname = rp[1].as_string();
			pgt.typnamespace = rp[2].as_oid();
			pgt.typowner = rp[3].as_oid();
			pgt.typlen = rp[4].as_int();
			pgt.typbyval = rp[5].as_bool();
			pgt.typtype = rp[6].c_str()[0];
			pgt.typisdefined = rp[7].as_bool();
			pgt.typdelim = rp[8].c_str()[0];
			pgt.typrelid = rp[9].as_oid();
			pgt.typelem = rp[10].as_oid();
			 

			config[seg].pg_type[pgt.oid] = pgt;
		}
	}

}

void get_expected_row_counts(size_t& expected_namespace_size, size_t & expected_class_size, size_t & expected_attribute_size,
		size_t& expected_type_size, const vector<gp_config_info> & config, dbresult::size_type segdbcount )
{
	// Assuming only a small amount of corruption (one or two segdbs with corruption, or everybody the same corruption) we can
	// use the average rowcount as what we think should be "right".
	size_t total_namespace_size = 0;
	size_t total_class_size = 0;
	size_t total_attribute_size = 0;
	size_t total_type_size = 0;
	size_t vsegs = 0;
	
	// Due to bug in CREATE EXTERNAL TABLE, master has less toast tables than the others.  So, don't look at him
	for (seg = 1; seg < segdbcount; seg++)
	{
		if (config[seg].valid || seg==0) // always count the master
		{
			total_namespace_size += config[seg].pg_namespace_by_oid.size();
			total_class_size += config[seg].pg_class.size();
			total_attribute_size += config[seg].pg_attribute.size();
			total_type_size += config[seg].pg_type.size();
			vsegs ++;
		}
	}
	
	expected_namespace_size = static_cast<size_t>(static_cast<double>(total_namespace_size)/vsegs + 0.5);
	expected_class_size = static_cast<size_t>(static_cast<double>(total_class_size)/vsegs + 0.5);
	expected_attribute_size = static_cast<size_t>(static_cast<double>(total_attribute_size)/vsegs + 0.5);
	expected_type_size = static_cast<size_t>(static_cast<double>(total_type_size)/vsegs + 0.5);
}

void usage()
{
	cout << "Usage:\n";
	cout << " gpcheckdb [OPTIONS]... \n";
	cout << "  -d DBNAME       specify database name to connect to \n";
	cout << "  -h HOSTNAME     database server host or socket directory \n";
	cout << "  -p PORT         database server port \n";
	cout << "  -U NAME         database user name \n";
	cout << "  -v      verbose \n";
	cout << "  -?      give this usage info\n";
}

int main(int argc, char **argv)
{
	bool verbose = false;

	static struct option long_options[] =
	{
	{ "dbname", required_argument, NULL, 'd' },
	{ "host", required_argument, NULL, 'h' },
	{ "port", required_argument, NULL, 'p' },
	{ "username", required_argument, NULL, 'U' },
	{ "verbose", no_argument, NULL, 'v' },
	{ "version", no_argument, NULL, 'V' },
	{ "password", no_argument, NULL, 'W' },
	{ "help", no_argument, NULL, '?' },
	{ NULL, 0, NULL, 0 } };

	int optindex;
	extern char *optarg;
	extern int optind;

	string dbname;
	string host;
	string port;
	string username;
	string password;

	int c;
	while ((c= getopt_long(argc, argv, "d:h:p:U:vVW?", long_options, &optindex))
			!= -1)
	{
		switch (c)
		{

		case 'd':
			dbname = optarg;
			break;
		case 'h':
			host = optarg;
			break;
		case 'p':
			port = optarg;
			break;

		case 'U':
			username = optarg;
			break;
		case 'v':
			verbose = true;
			break;
		case 'V':
			cerr << argv[0] << " version 0" << endl;
			exit(EXIT_SUCCESS);
			break;
		case 'W':
			password = optarg;
			break;
		case '?':
			/* Actual help option given */
			if (strcmp(argv[optind - 1], "-?") == 0 || strcmp(argv[optind - 1],
					"--help") == 0)
			{
				usage();
				exit(EXIT_SUCCESS);
			}
			/* unknown option reported by getopt */
			else
			{
				cerr << "unknown option" << endl;
				exit(EXIT_FAILURE);
			}
			break;
		default:
			cerr << "unknown option " << (char)c << endl;
			exit(EXIT_FAILURE);
			break;
		}
	}

	/*
	 * if we still have arguments, use it as the database name and username like psql does
	 */
	while (argc - optind >= 1)
	{

		if (dbname.empty())
			dbname = argv[optind];
		else if (username.empty())
			username = argv[optind];
		else
			cerr << argv[0] << ": warning: extra command-line argument \""
					<< argv[optind] << "\" ignored\n";

		optind++;
	}

	// common connection info for all segments
	string conninfo;
	if (!dbname.empty())
		conninfo += " dbname=" + dbname;
	if (!username.empty())
		conninfo += " user=" + username;
	if (!password.empty())
		conninfo += " password=" + password;

	// master DB connection info
	string masterconninfo(conninfo);
	if (!host.empty())
		masterconninfo += " host=" + host;
	if (!port.empty())
		masterconninfo += " port=" + port;
	
 
	dbconnection con;
	
	try
	{
		con.connect(masterconninfo, true);	// connect to the master in utility mode
	}
	catch(connection_exception & ce)
	{
		cerr << "Could not connect to the GPDB system:  " << ce.what() << endl;
		exit(EXIT_FAILURE);
	}
	
	//  Get the configuration, except for the standby master if any, as we won't be able to talk to the standby.
	dbresult result = con.exec("select * from pg_catalog.gp_configuration where dbid = 1 or content >= 0 order by content, definedprimary desc, dbid");

	dbresult::size_type segdbcount = result.size();

	// config[0] is the master DB.

	vector<gp_config_info> config(segdbcount);
	
	for (dbresult::size_type i = 0; i < segdbcount; ++i)
	{
		config[i].content = result[i][0].as_int();
		config[i].definedprimary = result[i][1].as_bool();
		config[i].dbid =result[i][2].as_int();
		config[i].isprimary = result[i][3].as_bool();
		config[i].valid = result[i][4].as_bool();
		config[i].hostname = result[i][5].as_string();
		config[i].port = result[i][6].as_string();
	}

	// Already have a connection to the master, just save it, rather than starting a new connection.
	config[0].conn = con; 	// con is useless after this, control transferred to config[0].conn
 
	mastername = config[0].segname();

	if (verbose)
		cout << "Connected to " << mastername << " and configuration read\n";

	result =config[0].conn.exec("select version()");
	string masterversion = result[0][0].as_string();
	
	if (masterversion.empty())
		cerr << "Could not get version info from master\n";

	if (verbose)
		cout << masterversion << endl;
	
	if (verbose)
		cout << "Reading catalog information\n";
	
	// Read pg_namespace, pg_class, pg_attribute, and pg_type from each segment database
	read_catalog_info(config, segdbcount, conninfo, masterversion);

	if (verbose)
		cout << "Catalog information read in.\n";
	
	// Right now, use the master DB catalog as the gold standard.
	// We could pick any segDB... Perhaps we need to let the user say which he thinks isn't corrupt.
	
	reference_namespace_map_by_name = &config[0].pg_namespace_by_name;
	reference_namespace_map_by_oid = &config[0].pg_namespace_by_oid;
	reference_class_map = &config[0].pg_class;
	reference_attribute_map = &config[0].pg_attribute;
	reference_type_map = &config[0].pg_type;
	
	if (verbose)
			cout << "Checking master DB catalog info for corruption.\n";
	
	seg = 0;
	segname = mastername;
	
	// For each table, check the cross reference to other tables on this same segment.

	for_each(config[seg].pg_class.begin(), config[seg].pg_class.end(), verify_pg_class_to_pg_namespace());

	for_each(config[seg].pg_attribute.begin(), config[seg].pg_attribute.end(), verify_pg_attribute_to_pg_class());
	
	for_each(config[seg].pg_class.begin(), config[seg].pg_class.end(), verify_pg_class_to_pg_attribute());
	
	for_each(config[seg].pg_type.begin(), config[seg].pg_type.end(), verify_pg_type_to_pg_class());
	
	if (verbose)
		cout << "Checking all segment DBs against the master.\n";
	

	//for_each(config[1].pg_namespace.begin(), config[1].pg_namespace.end(), print_pg_namespace);
	

	size_t expected_namespace_size;
	size_t expected_class_size;
	size_t expected_attribute_size;
	size_t expected_type_size;
	
	get_expected_row_counts(expected_namespace_size,expected_class_size,
			expected_attribute_size,expected_type_size,
			config, segdbcount);

	
	for (seg = 0; seg < segdbcount; seg++)
	{
		if (config[seg].pg_namespace_by_oid.size() != expected_namespace_size)
			cout << "pg_namespace row count on seg " << config[seg].segname()
					<< " is " << config[seg].pg_namespace_by_oid.size()
					<< " expected is " << expected_namespace_size << endl;

		if (config[seg].pg_class.size() != expected_class_size)
			cout << "pg_class row count on seg " <<  config[seg].segname() << " is "
					<< config[seg].pg_class.size() << " expected is "
					<< expected_class_size << endl;
		
		if (config[seg].pg_attribute.size() != expected_attribute_size)
			cout << "pg_attribute row count on seg " <<  config[seg].segname() << " is "
					<< config[seg].pg_attribute.size() << " expected is "
					<< expected_attribute_size << endl;

		if (config[seg].pg_type.size() != expected_type_size)
			cout << "pg_type row count on seg " <<  config[seg].segname() << " is "
					<< config[seg].pg_type.size() << " expected is "
					<< expected_type_size << endl;
	}

	// For each segment, check that the table is consistent with the reference segment database (usually the master).
	
	for (seg = 1; seg < segdbcount; seg++)
	{
		segname = config[seg].segname();
		if (verbose)
			cout << "Checking " << segname << endl;
		
		current_class_map = &config[seg].pg_class;

		// Each loop does basically a "left outer join" type operation to compare to the reference.
		for_each(config[seg].pg_namespace_by_name.begin(), config[seg].pg_namespace_by_name.end(), bind1st(verify_pg_namespace_against_reference(),&config[reference_host].pg_namespace_by_name)); 

		// Each loop does basically a "left outer join" type operation to compare to the reference.
		for_each(config[seg].pg_class.begin(), config[seg].pg_class.end(), bind1st(verify_pg_class_against_reference(),&config[reference_host].pg_class));
		// We really need full outer join results, so do the join the other way to pick up the missing results.
		for_each(config[reference_host].pg_class.begin(), config[reference_host].pg_class.end(), bind1st(verify_reference_against_pg_class(),&config[seg].pg_class));
	 
		for_each(config[seg].pg_attribute.begin(), 	config[seg].pg_attribute.end(), bind1st(verify_pg_attribute_against_reference(),&config[reference_host].pg_attribute));
		for_each(config[reference_host].pg_attribute.begin(), config[reference_host].pg_attribute.end(), bind1st(verify_reference_against_pg_attribute(),&config[seg].pg_attribute));

		for_each(config[seg].pg_type.begin(), config[seg].pg_type.end(),	bind1st(verify_pg_type_against_reference(),&config[reference_host].pg_type));
		for_each(config[reference_host].pg_type.begin(), config[reference_host].pg_type.end(), bind1st(verify_reference_against_pg_type(),&config[seg].pg_type));
	}

	if (corrupted)
	{
		cerr << "Catalog appears to be corrupted" << endl;
		exit(EXIT_FAILURE);
	}
	else
		cout << "Catalog OK " << endl;

}
