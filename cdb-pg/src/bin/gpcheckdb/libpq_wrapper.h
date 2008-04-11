#ifndef LIBPQ_WRAPPER_H_
#define LIBPQ_WRAPPER_H_

#include <exception>
#include <stdexcept>
#include <iostream>
#include <iterator>
#include <string>

namespace libpq
{
#include "libpq-fe.h"
}

namespace libpq_wrapper
{

// Chuck's ugly but really simple and lightweight c++ wrapper around libpq.

// The advantage to using this rather than something like libpqxx is I don't need to rely on any third-party libraries (just libpq itself).
// And yes, it's not done yet.  I just implemented what I needed for the moment.

// First, some exceptions to throw when things go wrong.


class connection_exception : public std::runtime_error
{
public:
	connection_exception(const char * w) :
		std::runtime_error(w)
	{
	}
};

class query_exception : public std::runtime_error
{
public:
	query_exception(const char * w) :
		std::runtime_error(w)
	{
	}
};

using namespace std;
using namespace libpq;
class dbrow;
class dbresult;

// We want to view the PGresult as two levels of random-access containers:  First, a container of rows,
// and then each row is a container of fields.

// But, libpq "protects" us from knowing the internal structure of pg_result, which would make it easy.
// If we had access to this private data, we could make our iterators be actual pointers.
// But, since we don't, we need to represent iterators as a combination of the PGresult pointer, plus the row (and then column).

// Also, since the pg_result is opaque, we can't change it, so this is a "const" set of containers and interators.

// One field of a result.   Everything is const, as we can't change the result.
class dbfield
{
	const PGresult * res; // Should this be dbrow*?
	const int tup;
	const int col;
public:
	typedef size_t size_type;
	dbfield(const PGresult * r, int t, int c) :
		res(r), tup(t), col(c)
	{
	}

	const char * c_str() const
	{
		return PQgetvalue(res, tup, col);
	}
	bool is_null() const
	{
		return static_cast<bool>(PQgetisnull(res, tup, col));
	}
	const char * name() const
	{
		return PQfname(res, col);
	}
	Oid type() const
	{
		return PQftype(res, col);
	}
	size_type num() const
	{
		return static_cast<size_type>(col);
	}
	size_type size() const
	{
		return static_cast<size_type>(PQgetlength(res, tup, col) );
	}

	const char * operator*() const
	{
		return c_str();
	} // Is this a good idea?

	int as_int() const
	{
		const char * v = c_str();
		int temp = 0;
		// Could check oid = 20, 21, 23, but perhaps we want to use this on text?
		if (strlen(v) > 0)
		{
			if ((v[0] < '0' || v[0] > '9' ) && v[0] != '-' && v[0] != ' ')
				throw query_exception("as_int() used on non-numeric field");
		}
		istringstream t(v);
		t >> temp;
		return temp;
	}
	Oid as_oid() const
	{
		if (type() != 26 /* OIDOID */ && type() != 24 /* REGPROC */)
			throw query_exception("as_oid() used on non-oid field");
		return as_int();
	}
	double as_double() const
	{
		if (type() != 701 /* FLOAT8 */ && type() != 700 /* FLOAT4 */)
			throw query_exception("as_double() used on non-float field");
		double temp = 0.0;
		istringstream t( c_str());
		t >> temp;
		return temp;
	}
	bool as_bool() const
	{
		if (type() != 16)
			throw query_exception("as_bool() used on non-bool field");
		return c_str()[0] == 't';
	}
	std::string as_string() const
	{
		return std::string(c_str());
	}

	// Are these useful?  need to handle nulls here.
	bool operator==(const dbfield & other) const
	{
		return strcmp(c_str(), other.c_str()) == 0;
	}
	bool operator>(const dbfield & other) const
	{
		return strcmp(c_str(), other.c_str()) > 0;
	}
	bool operator<(const dbfield & other) const
	{
		return strcmp(c_str(), other.c_str()) < 0;
	}

	friend std::ostream & operator<<(std::ostream & os, const dbfield & f)
	{
		if (f.is_null() )
			os << "<null>";
		else
			os << f.c_str();
		return os;
	}
};

// Let us interate through fields in a row/tuple.
// These iterators are const iterators, since we can't change the pg_result struct.

class dbfield_iterator :
	public std::iterator<std::random_access_iterator_tag, dbfield>
{
	const PGresult * res;
	const int tup;
	int col;
public:
	dbfield_iterator(const PGresult * r, int t, int c) :
		res(r), tup(t), col(c)
	{
	}
	const value_type operator*() const
	{
		return (dbfield(res, tup, col));
	}
	//const value_type operator[](int c) const
	//{
	//	return (dbfield(res, tup, c));
	//	}
	dbfield_iterator & operator++()
	{
		col++;
		return (*this);
	}
	bool operator==(const dbfield_iterator & other) const
	{
		return tup == other.tup && col == other.col;
	}
	bool operator!=(const dbfield_iterator & other) const
	{
		return tup != other.tup || col != other.col;
	}
	bool operator>(const dbfield_iterator & other) const
	{
		return tup > other.tup || col > other.col;
	}
	bool operator<(const dbfield_iterator & other) const
	{
		return tup < other.tup || col < other.col;
	}
};

// one row (tuple) from the result set.   A container of fields.
class dbrow
{
	const PGresult * res;
	const int tup;
public:
	typedef dbfield value_type;
	typedef size_t size_type;
	typedef ptrdiff_t difference_type;
	typedef dbfield_iterator iterator;
	typedef dbfield_iterator const_iterator;
	typedef std::allocator<dbfield> allocator_type;

	dbrow(const PGresult * r, int t) :
		res(r), tup(t)
	{
	}
	//dbrow(dbrow & dbr): res(dbr.release() ), tup(dbr.tup)
	//{
	//}
	size_type size() const
	{
		return PQnfields(res);
	}
	size_type max_size() const
	{
		return size();
	}

	const value_type operator[](size_type c) const
	{
		return value_type(res, tup, c);
	}
	const value_type at(size_type c) const
	{
		return value_type(res, tup, c);
	}
	const value_type operator[](int c) const
	{
		return value_type(res, tup, c);
	}
	const value_type at(int c) const
	{
		return value_type(res, tup, c);
	}

	// access by column name
	const value_type operator[](const char * cn) const
	{
		return value_type(res, tup, PQfnumber(res, cn));
	}
	const value_type at(const char * cn) const
	{
		return value_type(res, tup, PQfnumber(res, cn));
	}

	const value_type front() const
	{
		return value_type(res, tup, 0);
	}

	const value_type back() const
	{
		return value_type(res, tup, size()-1);
	}

	const_iterator begin() const
	{
		return const_iterator(res, tup, 0);
	}
	const_iterator end() const
	{
		return const_iterator(res, tup, size());
	}

	bool operator==(const dbrow & other) const
	{
		return res == other.res && tup == other.tup;
	}
};

// dbrow iterator.  This is a const_iterator, since we can't change the pg_result
class dbrow_iterator :
	public std::iterator<std::random_access_iterator_tag, dbrow>
{
	const PGresult * res;
	int tup;
public:
	dbrow_iterator(const PGresult * r, int t) :
		res(r), tup(t)
	{
		if (res == 0)
			throw query_exception("dbrow_iterator initialized with null PGresult");
	}
	const value_type operator*() const
	{
		if (res == 0)
			throw query_exception("dbrow_iterator isn't initalized");
		return (dbrow(res, tup));
	}
	const dbfield operator[](int c) const
	{
		return (dbfield(res, tup, c));
	}
	dbrow_iterator & operator++()
	{
		tup++;
		return (*this);
	}
	bool operator==(const dbrow_iterator & other) const
	{
		return tup == other.tup;
	}
	bool operator!=(const dbrow_iterator & other) const
	{
		return tup != other.tup;
	}
	bool operator>(const dbrow_iterator & other) const
	{
		return tup > other.tup;
	}
	bool operator<(const dbrow_iterator & other) const
	{
		return tup < other.tup;
	}
};

// a result set.  A container of dbrow types.
class dbresult
{
	PGresult * res;
public:
	typedef dbrow value_type;
	typedef size_t size_type;
	typedef ptrdiff_t difference_type;
	typedef dbrow_iterator iterator;
	typedef dbrow_iterator const_iterator;
	typedef std::allocator<dbrow> allocator_type;

	dbresult() :
		res(0)
	{
	}
	dbresult(PGresult * r) :
		res(r)
	{
	}
	~dbresult()
	{
		if (res != 0)
			PQclear(res);
		res = 0;
	}

	/*
	 dbresult(dbresult& dbr)
	 {
	 res = dbr.release();
	 }*/

	size_type size() const
	{
		if (res == 0)
			throw query_exception("dbresult isn't initalized");
		return PQntuples(res);
	}
	size_type max_size() const
	{
		return size();
	}

	const value_type operator[](size_type r) const
	{
		return value_type(res, static_cast<int>(r));
	}
	const value_type operator[](int r) const
	{
		return value_type(res, r);
	}
	const value_type at(size_type r) const
	{
		if (res == 0)
			throw query_exception("dbresult isn't initalized");
		return value_type(res, r);
	}
	const value_type at(int r) const
	{
		if (res == 0)
			throw query_exception("dbresult isn't initalized");
		return value_type(res, r);
	}

	Oid column_type(int c) const
	{
		return PQftype(res, c);
	}

	const value_type front() const
	{
		return value_type(res, 0);
	}
	const value_type back() const
	{
		return value_type(res, size()-1);
	}

	const_iterator begin() const
	{
		if (res == 0)
			throw query_exception("dbresult isn't initalized");
		return const_iterator(res, 0);
	}
	const_iterator end() const
	{
		if (res == 0)
			throw query_exception("dbresult isn't initalized");
		return const_iterator(res, size());
	}

	// give up control of the PGresult to somebody else.
	PGresult * release()
	{
		PGresult * temp_res = res;
		res = 0;
		return temp_res;
	}

	// get rid of any current PGresult we have, assign new one.
	void reset(PGresult * r = 0) throw()
	{
		if (r != res)
		{
			if (res !=0)
				PQclear(res);
			res = r;
		}
	}

	dbresult& operator=(dbresult& other) throw()
	{
		reset(other.release());
		return *this;
	}
	dbresult & operator=(dbresult other) throw()
	{
		reset(other.release());
		return *this;
	}

};

class dbconnection
{
	PGconn * conn;
public:
	void connect(const std::string & conninfo, bool utilitymode = false)
	{
		std::string cinfo(conninfo);
		if (utilitymode)
			cinfo += " options='-c gp_session_role=utility' ";
		conn = PQconnectdb(cinfo.c_str());
		/* Check to see that the backend connection was successfully made */
		if (PQstatus(conn) != CONNECTION_OK)
			throw connection_exception(PQerrorMessage(conn));

	}
	dbconnection() :
		conn(0)
	{
	}
	dbconnection(const std::string & conninfo)
	{
		connect(conninfo, false);
	}

	~dbconnection()
	{
		if (conn != 0)
			PQfinish(conn);
		conn = 0;
	}
	// give up control of the PGresult to somebody else.
	PGconn * release()
	{
		PGconn * temp_conn = conn;
		conn = 0;
		return temp_conn;
	}

	// get rid of any current PGresult we have, assign new one.
	void reset(PGconn * c = 0) throw()
	{
		if (c != conn)
		{
			if (conn !=0)
				PQfinish(conn);
			conn =c;
		}
	}
	dbconnection& operator=(dbconnection& other) throw()
	{
		reset(other.release());
		return *this;
	}

	dbresult exec(const std::string & query)
	{
		PGresult * res = PQexec(conn, query.c_str());
		if (PQresultStatus(res) != PGRES_TUPLES_OK && PQresultStatus(res) != PGRES_COMMAND_OK)
			throw query_exception(PQerrorMessage(conn));

		return dbresult(res);
	}
};

}

#endif /*LIBPQ_WRAPPER_H_*/
