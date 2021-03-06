/** @file dbcheck.cc
 * @brief Check the consistency of a database or table.
 */
/* Copyright 1999,2000,2001 BrightStation PLC
 * Copyright 2002,2003,2004,2005,2006,2007,2008,2009,2010,2011,2012,2013,2014,2015 Olly Betts
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301
 * USA
 */

#include <config.h>
#include "xapian/database.h"

#include "xapian/error.h"

#ifdef XAPIAN_HAS_GLASS_BACKEND
#include "glass/glass_changes.h"
#include "glass/glass_database.h"
#include "glass/glass_dbcheck.h"
#include "glass/glass_version.h"
#endif
#ifdef XAPIAN_HAS_CHERT_BACKEND
#include "chert/chert_database.h"
#include "chert/chert_dbcheck.h"
#include "chert/chert_types.h"
#include "chert/chert_version.h"
#endif

#include "filetests.h"
#include "stringutils.h"

#include <ostream>
#include <stdexcept>

using namespace std;

// FIXME: We don't currently cross-check wdf between postlist and termlist.
// It's hard to see how to efficiently.  We do cross-check doclens, but that
// "only" requires (4 * last_docid()) bytes.

#if defined XAPIAN_HAS_CHERT_BACKEND || defined XAPIAN_HAS_GLASS_BACKEND
static void
reserve_doclens(vector<Xapian::termcount>& doclens, Xapian::docid last_docid,
		ostream * out)
{
    if (last_docid >= 0x40000000ul / sizeof(Xapian::termcount)) {
	// The memory block needed by the vector would be >= 1GB.
	if (out)
	    *out << "Cross-checking document lengths between the postlist and "
		    "termlist tables would use more than 1GB of memory, so "
		    "skipping that check" << endl;
	return;
    }
    try {
	doclens.reserve(last_docid + 1);
    } catch (const std::bad_alloc &) {
	// Failed to allocate the required memory.
	if (out)
	    *out << "Couldn't allocate enough memory for cross-checking document "
		    "lengths between the postlist and termlist tables, so "
		    "skipping that check" << endl;
    } catch (const std::length_error &) {
	// There are too many elements for the vector to handle!
	if (out)
	    *out << "Couldn't allocate enough elements for cross-checking document "
		    "lengths between the postlist and termlist tables, so "
		    "skipping that check" << endl;
    }
}
#endif

namespace Xapian {

size_t
Database::check(const string & path, int opts, std::ostream *out)
{
    if (!out) {
	// If we have nowhere to write output, then disable all the options
	// which only affect what we output.
	opts &= Xapian::DBCHECK_FIX;
    }
    vector<Xapian::termcount> doclens;
    size_t errors = 0;
    struct stat sb;
    if (stat((path + "/iamchert").c_str(), &sb) == 0) {
#ifndef XAPIAN_HAS_CHERT_BACKEND
	(void)opts;
	(void)out;
	throw Xapian::FeatureUnavailableError("Chert database support isn't enabled");
#else
	// Check a whole chert database directory.
	// If we can't read the last docid, set it to its maximum value
	// to suppress errors.
	Xapian::docid db_last_docid = static_cast<Xapian::docid>(-1);
	chert_revision_number_t rev = 0;
	chert_revision_number_t * rev_ptr = &rev;
	try {
	    // Open at the lower level so we can get the revision number.
	    ChertDatabase db(path);
	    db_last_docid = db.get_lastdocid();
	    reserve_doclens(doclens, db_last_docid, out);
	    rev = db.get_revision_number();
	} catch (const Xapian::Error & e) {
	    // Ignore so we can check a database too broken to open.
	    if (out)
		*out << "Database couldn't be opened for reading: "
		     << e.get_description()
		     << "\nContinuing check anyway" << endl;
	    ++errors;
	}

	size_t pre_table_check_errors = errors;

	// This is a chert directory so try to check all the btrees.
	// Note: it's important to check "termlist" before "postlist" so
	// that we can cross-check the document lengths; also we check
	// "record" first as that's the last committed, so has the most
	// reliable rootblock revision in DBCHECK_FIX mode.
	const char * tables[] = {
	    "record", "termlist", "postlist", "position",
	    "spelling", "synonym"
	};
	for (const char **t = tables;
	     t != tables + sizeof(tables)/sizeof(tables[0]); ++t) {
	    string table(path);
	    table += '/';
	    table += *t;
	    if (out)
		*out << *t << ":\n";
	    if (strcmp(*t, "record") != 0 && strcmp(*t, "postlist") != 0) {
		// Other tables are created lazily, so may not exist.
		if (!file_exists(table + ".DB")) {
		    if (out) {
			if (strcmp(*t, "termlist") == 0) {
			    *out << "Not present.\n";
			} else {
			    *out << "Lazily created, and not yet used.\n";
			}
			*out << endl;
		    }
		    continue;
		}
	    }
	    errors += check_chert_table(*t, table, rev_ptr, opts, doclens,
					db_last_docid, out);
	}

	if (errors == pre_table_check_errors && (opts & Xapian::DBCHECK_FIX)) {
	    // Check the version file is OK and if not, recreate it.
	    ChertVersion iam(path);
	    try {
		iam.read_and_check();
	    } catch (const Xapian::DatabaseError &) {
		iam.create();
	    }
	}
#endif
    } else if (stat((path + "/iamglass").c_str(), &sb) == 0) {
#ifndef XAPIAN_HAS_GLASS_BACKEND
	(void)opts;
	(void)out;
	throw Xapian::FeatureUnavailableError("Glass database support isn't enabled");
#else
	// Check a whole glass database directory.
	try {
	    // Check if the database can actually be opened.
	    Xapian::Database db(path);
	} catch (const Xapian::Error & e) {
	    // Continue - we can still usefully look at how it is broken.
	    if (out)
		*out << "Database couldn't be opened for reading: "
		     << e.get_description()
		     << "\nContinuing check anyway" << endl;
	    ++errors;
	}

	GlassVersion version_file(path);
	version_file.read();
	for (glass_revision_number_t r = version_file.get_revision(); r != 0; --r) {
	    string changes_file = path;
	    changes_file += "/changes";
	    changes_file += str(r);
	    if (file_exists(changes_file))
		GlassChanges::check(changes_file);
	}

	Xapian::docid db_last_docid = version_file.get_last_docid();
	reserve_doclens(doclens, db_last_docid, out);

	// This is a glass directory so try to check all the btrees.
	// Note: it's important to check termlist before postlist so
	// that we can cross-check the document lengths.
	const char * tables[] = {
	    "docdata", "termlist", "postlist", "position",
	    "spelling", "synonym"
	};
	for (const char **t = tables;
	     t != tables + sizeof(tables)/sizeof(tables[0]); ++t) {
	    errors += check_glass_table(*t, path, version_file, opts, doclens,
					db_last_docid, out);
	}
#endif
    } else {
	if (stat((path + "/iamflint").c_str(), &sb) == 0) {
	    // Flint is no longer supported as of Xapian 1.3.0.
	    throw Xapian::FeatureUnavailableError("Flint database support was removed in Xapian 1.3.0");
	}
	if (stat((path + "/iambrass").c_str(), &sb) == 0) {
	    // Brass was renamed to glass as of Xapian 1.3.2.
	    throw Xapian::FeatureUnavailableError("Brass database support was removed in Xapian 1.3.2");
	}
	if (stat((path + "/record_DB").c_str(), &sb) == 0) {
	    // Quartz is no longer supported as of Xapian 1.1.0.
	    throw Xapian::FeatureUnavailableError("Quartz database support was removed in Xapian 1.1.0");
	}
	// Just check a single Btree.  If it ends with ".", ".DB", or ".glass",
	// trim that so the user can do xapian-check on "foo", "foo.", "foo.DB",
	// "foo.glass", etc.
	enum { UNKNOWN, CHERT, GLASS } backend = UNKNOWN;
	string filename = path;
	if (endswith(filename, '.')) {
	    filename.resize(filename.size() - 1);
	} else if (endswith(filename, ".DB")) {
	    filename.resize(filename.size() - CONST_STRLEN(".DB"));
	    backend = CHERT;
	} else if (endswith(filename, ".glass")) {
	    filename.resize(filename.size() - CONST_STRLEN(".glass"));
	    backend = GLASS;
	}

	if (backend == UNKNOWN) {
	    if (stat((filename + ".DB").c_str(), &sb) == 0) {
		// It could also be flint or brass, but we check for those below.
		backend = CHERT;
	    } else if (stat((filename + ".glass").c_str(), &sb) == 0) {
		backend = GLASS;
	    } else {
		throw Xapian::DatabaseError("Not a Xapian database or database table");
	    }
	}

	size_t p = filename.find_last_of('/');
#if defined __WIN32__ || defined __OS2__
	if (p == string::npos) p = 0;
	p = filename.find_last_of('\\', p);
#endif
	if (p == string::npos) p = 0; else ++p;

	string dir(filename, 0, p);

	string tablename;
	while (p != filename.size()) {
	    tablename += tolower(static_cast<unsigned char>(filename[p++]));
	}

	if (backend == GLASS) {
#ifndef XAPIAN_HAS_GLASS_BACKEND
	    throw Xapian::FeatureUnavailableError("Glass database support isn't enabled");
#else
	    GlassVersion version_file(dir);
	    version_file.read();
	    // Set the last docid to its maximum value to suppress errors.
	    Xapian::docid db_last_docid = static_cast<Xapian::docid>(-1);
	    errors = check_glass_table(tablename.c_str(), dir,
				       version_file, opts,
				       doclens, db_last_docid, out);
#endif
	} else if (backend == CHERT) {
	    // Flint and brass also used the extension ".DB", so check that we
	    // haven't been passed a single table in a flint or brass database.
	    if (stat((dir + "/iamflint").c_str(), &sb) == 0) {
		// Flint is no longer supported as of Xapian 1.3.0.
		throw Xapian::FeatureUnavailableError("Flint database support was removed in Xapian 1.3.0");
	    }
	    if (stat((dir + "/iambrass").c_str(), &sb) == 0) {
		// Brass was renamed to glass as of Xapian 1.3.2.
		throw Xapian::FeatureUnavailableError("Brass database support was removed in Xapian 1.3.2");
	    }
#ifndef XAPIAN_HAS_CHERT_BACKEND
	    throw Xapian::FeatureUnavailableError("Chert database support isn't enabled");
#else
	    // Set the last docid to its maximum value to suppress errors.
	    Xapian::docid db_last_docid = static_cast<Xapian::docid>(-1);
	    errors = check_chert_table(tablename.c_str(), filename, NULL, opts,
				       doclens, db_last_docid, out);
#endif
	}
    }
    return errors;
}

}
