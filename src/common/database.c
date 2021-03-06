/*
    This file is part of darktable,
    copyright (c) 2011 henrik andersson.
    copyright (c) 2012 tobias ellinghaus.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "common/darktable.h"
#include "common/debug.h"
#include "common/database.h"
#include "control/control.h"
#include "control/conf.h"

#include <sqlite3.h>
#include <glib.h>
#include <gio/gio.h>

typedef struct dt_database_t
{
  gboolean is_new_database;
  gboolean already_locked;

  /* database filename */
  gchar *dbfilename;

  /* ondisk DB */
  sqlite3 *handle;
} dt_database_t;


/* migrates database from old place to new */
static void _database_migrate_to_xdg_structure();

/* delete old mipmaps files */
static void _database_delete_mipmaps_files();

gboolean dt_database_is_new(const dt_database_t *db)
{
  return db->is_new_database;
}

dt_database_t *dt_database_init(char *alternative)
{
  /* migrate default database location to new default */
  _database_migrate_to_xdg_structure();

  /* delete old mipmaps files */
  _database_delete_mipmaps_files();

  /* lets construct the db filename  */
  gchar * dbname = NULL;
  gchar dbfilename[DT_MAX_PATH_LEN] = {0};
  gchar datadir[DT_MAX_PATH_LEN] = {0};

  dt_loc_get_user_config_dir(datadir, DT_MAX_PATH_LEN);

  if ( alternative == NULL )
  {
    dbname = dt_conf_get_string ("database");
    if(!dbname)               snprintf(dbfilename, DT_MAX_PATH_LEN, "%s/library.db", datadir);
    else if(dbname[0] != '/') snprintf(dbfilename, DT_MAX_PATH_LEN, "%s/%s", datadir, dbname);
    else                      snprintf(dbfilename, DT_MAX_PATH_LEN, "%s", dbname);
  }
  else
  {
    snprintf(dbfilename, DT_MAX_PATH_LEN, "%s", alternative);

    GFile *galternative = g_file_new_for_path(alternative);
    dbname = g_file_get_basename (galternative);
    g_object_unref(galternative);
  }

  /* create database */
  dt_database_t *db = (dt_database_t *)g_malloc(sizeof(dt_database_t));
  memset(db,0,sizeof(dt_database_t));
  db->dbfilename = g_strdup(dbfilename);
  db->is_new_database = FALSE;

  /* test if databasefile is available */
  if(!g_file_test(dbfilename, G_FILE_TEST_IS_REGULAR))
    db->is_new_database = TRUE;

  /* opening / creating database */
  if(sqlite3_open(db->dbfilename, &db->handle))
  {
    fprintf(stderr, "[init] could not find database ");
    if(dbname) fprintf(stderr, "`%s'!\n", dbname);
    else       fprintf(stderr, "\n");
    fprintf(stderr, "[init] maybe your %s/darktablerc is corrupt?\n",datadir);
    dt_loc_get_datadir(dbfilename, 512);
    fprintf(stderr, "[init] try `cp %s/darktablerc %s/darktablerc'\n", dbfilename,datadir);
    sqlite3_close(db->handle);
    g_free(dbname);
    g_free(db);
    return NULL;
  }

  /* having more than one instance of darktable using the same database is a bad idea */
  if(sqlite3_exec(db->handle, "delete from lock", NULL, NULL, NULL) > SQLITE_ERROR)
  {
    fprintf(stderr, "[init] database is locked, probably another process is already using it\n");
    sqlite3_close(db->handle);
    g_free(dbname);
    db->already_locked = TRUE;
    return db;
  }

  /* attach a memory database to db connection for use with temporary tables
     used during instance life time, which is discarded on exit.
  */
  sqlite3_exec(db->handle, "attach database ':memory:' as memory",NULL,NULL,NULL);

  sqlite3_exec(db->handle, "PRAGMA synchronous = OFF", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "PRAGMA journal_mode = MEMORY", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "PRAGMA page_size = 32768", NULL, NULL, NULL);

  g_free(dbname);
  return db;
}

void dt_database_destroy(const dt_database_t *db)
{
  sqlite3_close(db->handle);
  g_free((dt_database_t *)db);
}

sqlite3 *dt_database_get(const dt_database_t *db)
{
  return db->handle;
}

const gchar *dt_database_get_path(const struct dt_database_t *db)
{
  return db->dbfilename;
}

static void _database_migrate_to_xdg_structure()
{
  gchar dbfilename[DT_MAX_PATH_LEN]= {0};
  gchar *conf_db = dt_conf_get_string("database");

  gchar datadir[DT_MAX_PATH_LEN] = {0};
  dt_loc_get_datadir(datadir, DT_MAX_PATH_LEN);

  if (conf_db && conf_db[0] != '/')
  {
    char *homedir = getenv ("HOME");
    snprintf (dbfilename,DT_MAX_PATH_LEN,"%s/%s", homedir, conf_db);
    if (g_file_test (dbfilename, G_FILE_TEST_EXISTS))
    {
      fprintf(stderr, "[init] moving database into new XDG directory structure\n");
      char destdbname[DT_MAX_PATH_LEN]= {0};
      snprintf(destdbname,DT_MAX_PATH_LEN,"%s/%s",datadir,"library.db");
      if(!g_file_test (destdbname,G_FILE_TEST_EXISTS))
      {
        rename(dbfilename,destdbname);
        dt_conf_set_string("database","library.db");
      }
    }
  }

  g_free(conf_db);
}

/* delete old mipmaps files */
static void _database_delete_mipmaps_files()
{
  /* This migration is intended to be run only from 0.9.x to new cache in 1.0 */

  // Directory
  char cachedir[DT_MAX_PATH_LEN], mipmapfilename[DT_MAX_PATH_LEN];
  dt_loc_get_user_cache_dir(cachedir, sizeof(cachedir));

  snprintf(mipmapfilename, DT_MAX_PATH_LEN, "%s/mipmaps", cachedir);

  if(access(mipmapfilename, F_OK) != -1)
  {
    fprintf(stderr, "[mipmap_cache] dropping old version file: %s\n", mipmapfilename);
    unlink(mipmapfilename);

    snprintf(mipmapfilename, DT_MAX_PATH_LEN, "%s/mipmaps.fallback", cachedir);

    if(access(mipmapfilename, F_OK) != -1)
      unlink(mipmapfilename);
  }
}

gboolean dt_database_get_already_locked(const dt_database_t *db)
{
  return db->already_locked;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
