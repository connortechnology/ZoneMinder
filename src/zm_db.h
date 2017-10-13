//
// ZoneMinder Core Interfaces, $Date$, $Revision$
// Copyright (C) 2001-2008 Philip Coombes
// 
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
// 

#ifndef ZM_DB_H
#define ZM_DB_H

#include <mysql/mysql.h>

class zmDbRow {
    private:
        MYSQL_RES *result_set;
        MYSQL_ROW row;
    public:
        zmDbRow() { result_set = NULL; row = NULL; };
        MYSQL_RES *fetch( const char *query );
        zmDbRow( MYSQL_RES *, MYSQL_ROW *row );
        ~zmDbRow();

        char *operator[](unsigned int index) const {
            return row[index];
        }
};

#ifdef __cplusplus 
extern "C" {
#endif 
extern MYSQL dbconn;

extern int zmDbConnected;

void zmDbConnect();
void zmDbClose();

MYSQL_RES * zmDbFetch( const char *query );
zmDbRow *zmDbFetchOne( const char *query );

#ifdef __cplusplus 
} /* extern "C" */
#endif 

#endif // ZM_DB_H
