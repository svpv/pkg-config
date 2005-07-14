#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pkg.h"
#include "parse.h"

#ifdef HAVE_ALLOCA_H
# include <alloca.h>
#else
# ifdef _AIX
#  pragma alloca
# endif
#endif

#include <sys/types.h>
#include <dirent.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

static void verify_package (Package *pkg);

static GHashTable *packages = NULL;
static GHashTable *locations = NULL;
static GHashTable *globals = NULL;
static GSList *search_dirs = NULL;

gboolean disable_uninstalled = FALSE;

void
add_search_dir (const char *path)
{
  search_dirs = g_slist_prepend (search_dirs, g_strdup (path));
}

#define EXT_LEN 3

static gboolean
ends_in_dotpc (const char *str)
{
  int len = strlen (str);
  
  if (len > EXT_LEN &&
      str[len - 3] == '.' &&
      str[len - 2] == 'p' &&
      str[len - 1] == 'c')
    return TRUE;
  else
    return FALSE;
}

/* strlen ("uninstalled") */
#define UNINSTALLED_LEN 11

gboolean
name_ends_in_uninstalled (const char *str)
{
  int len = strlen (str);
  
  if (len > UNINSTALLED_LEN &&
      strcmp ((str + len - UNINSTALLED_LEN), "uninstalled") == 0)
    return TRUE;
  else
    return FALSE;
}


/* Look for .pc files in the given directory and add them into
 * locations, ignoring duplicates
 */
static void
scan_dir (const char *dirname)
{
  DIR *dir = opendir (dirname);
  struct dirent *dent;
  int dirnamelen = strlen (dirname);

  if (dirnamelen > 0 && dirname[dirnamelen-1] == '/')
    dirnamelen--;

  if (!dir)
    {
      debug_spew ("Cannot open directory '%s' in package search path: %s\n",
                  dirname, g_strerror (errno));
      return;
    }

  debug_spew ("Scanning directory '%s'\n", dirname);
  
  while ((dent = readdir (dir)))
    {
      int len = strlen (dent->d_name);

      if (ends_in_dotpc (dent->d_name))
        {
          char *pkgname = malloc (len - 2);

          debug_spew ("File '%s' appears to be a .pc file\n", dent->d_name);
          
	  strncpy (pkgname, dent->d_name, len - EXT_LEN);
          pkgname[len-EXT_LEN] = '\0';

          if (g_hash_table_lookup (locations, pkgname))
            {
              debug_spew ("File '%s' ignored, we already know about package '%s'\n", dent->d_name, pkgname);
              g_free (pkgname);
            }
          else
            {
              char *filename = g_malloc (dirnamelen + 1 + len + 1);
              strncpy (filename, dirname, dirnamelen);
              filename[dirnamelen] = '/';
              strcpy (filename + dirnamelen + 1, dent->d_name);
              
              g_hash_table_insert (locations, pkgname, filename);

              debug_spew ("Will find package '%s' in file '%s'\n",
                          pkgname, filename);
            }
        }
      else
        {
          debug_spew ("Ignoring file '%s' in search directory; not a .pc file\n",
                      dent->d_name);
        }
    }
}

void
package_init ()
{
  static gboolean initted = FALSE;

  if (!initted)
    {
      initted = TRUE;
      
      packages = g_hash_table_new (g_str_hash, g_str_equal);
      locations = g_hash_table_new (g_str_hash, g_str_equal);
      
      g_slist_foreach (search_dirs, (GFunc)scan_dir, NULL);
      scan_dir (PKGLIBDIR);
    }
}

static gboolean
file_readable (const char *path)
{
  FILE *f = fopen (path, "r");

  if (f != NULL)
    {
      fclose (f);
      return TRUE;
    }
  else
    return FALSE;
}


static Package *
internal_get_package (const char *name, gboolean warn, gboolean check_compat)
{
  Package *pkg = NULL;
  const char *location;
  
  pkg = g_hash_table_lookup (packages, name);

  if (pkg)
    return pkg;

  debug_spew ("Looking for package '%s'\n", name);
  
  /* treat "name" as a filename if it ends in .pc and exists */
  if ( ends_in_dotpc (name) )
    {
      debug_spew ("Considering '%s' to be a filename rather than a package name\n", name);
      location = name;
    }
  else
    {
      /* See if we should auto-prefer the uninstalled version */
      if (!disable_uninstalled &&
          !name_ends_in_uninstalled (name))
        {
          char *un;

          un = g_strconcat (name, "-uninstalled", NULL);

          pkg = internal_get_package (un, FALSE, FALSE);

          g_free (un);
          
          if (pkg)
            {
              debug_spew ("Preferring uninstalled version of package '%s'\n", name);
              return pkg;
            }
        }
      
      location = g_hash_table_lookup (locations, name);
    }
  
  if (location == NULL && check_compat)
    {
      pkg = get_compat_package (name);

      if (pkg)
        {
          debug_spew ("Returning values for '%s' from a legacy -config script\n",
                      name);
          
          return pkg;
        }
    }
      
  if (location == NULL)
    {
      if (warn)
        verbose_error ("Package %s was not found in the pkg-config search path.\n"
                       "Perhaps you should add the directory containing `%s.pc'\n"
                       "to the PKG_CONFIG_PATH environment variable\n",
                       name, name);

      return NULL;
    }

  debug_spew ("Reading '%s' from file '%s'\n", name, location);
  pkg = parse_package_file (location);
  
  if (pkg == NULL)
    {
      debug_spew ("Failed to parse '%s'\n", location);
      return NULL;
    }

  if (strstr (location, "uninstalled.pc"))
    pkg->uninstalled = TRUE;
  
  if (location != name)
    pkg->key = g_strdup (name);
  else
    {
      /* need to strip package name out of the filename */
      int len = strlen (name);
      const char *end = name + (len - EXT_LEN);
      const char *start = end;

      while (start != name && *start != G_DIR_SEPARATOR)
        --start;

      g_assert (end >= start);
      
      pkg->key = g_strndup (start, end - start);
    }

  verify_package (pkg);

  debug_spew ("Adding '%s' to list of known packages, returning as package '%s'\n",
              pkg->key, name);
  
  g_hash_table_insert (packages, pkg->key, pkg);

  return pkg;
}

Package *
get_package (const char *name)
{
  return internal_get_package (name, TRUE, TRUE);
}

static GSList*
string_list_strip_duplicates (GSList *list)
{
  GHashTable *table;
  GSList *tmp;
  GSList *nodups = NULL;
  
  table = g_hash_table_new (g_str_hash, g_str_equal);

  tmp = list;
  while (tmp != NULL)
    {
      if (g_hash_table_lookup (table, tmp->data) == NULL)
        {
          nodups = g_slist_prepend (nodups, tmp->data);
          g_hash_table_insert (table, tmp->data, tmp->data);
        }

      tmp = g_slist_next (tmp);
    }

  nodups = g_slist_reverse (nodups);
  
  g_hash_table_destroy (table);
  
  return nodups;
}

static GSList*
string_list_strip_duplicates_from_back (GSList *list)
{
  GHashTable *table;
  GSList *tmp;
  GSList *nodups = NULL;
  GSList *reversed;
  
  table = g_hash_table_new (g_str_hash, g_str_equal);

  reversed = g_slist_reverse (g_slist_copy (list));
  
  tmp = reversed;
  while (tmp != NULL)
    {
      if (g_hash_table_lookup (table, tmp->data) == NULL)
        {
          /* This unreverses the reversed list */
          nodups = g_slist_prepend (nodups, tmp->data);
          g_hash_table_insert (table, tmp->data, tmp->data);
        }

      tmp = g_slist_next (tmp);
    }

  g_slist_free (reversed);
  
  g_hash_table_destroy (table);
  
  return nodups;
}

static char *
string_list_to_string (GSList *list)
{
  GSList *tmp;
  GString *str = g_string_new ("");
  char *retval;
  
  tmp = list;
  while (tmp != NULL)
    {
      g_string_append (str, tmp->data);
      g_string_append_c (str, ' ');
      
      tmp = g_slist_next (tmp);
    }

  retval = str->str;
  g_string_free (str, FALSE);

  return retval;
}

typedef GSList *(* GetListFunc) (Package *pkg);

static GSList *
get_l_libs (Package *pkg)
{
  return pkg->l_libs;
}

static GSList *
get_L_libs (Package *pkg)
{
  return pkg->L_libs;
}

static GSList *
get_I_cflags (Package *pkg)
{
  return pkg->I_cflags;
}

static GSList *
get_conflicts (Package *pkg)
{
  return pkg->conflicts;
}

static GSList *
get_requires (Package *pkg)
{
  return pkg->requires;
}

static void
recursive_fill_list (Package *pkg, GetListFunc func, GSList **listp)
{
  GSList *tmp;
  GSList *copy;

  copy = g_slist_copy ((*func)(pkg));

  *listp = g_slist_concat (*listp, copy);
  
  tmp = pkg->requires;

  while (tmp != NULL)
    {
      recursive_fill_list (tmp->data, func, listp);

      tmp = g_slist_next (tmp);
    }
}

static gint
compare_req_version_names (gconstpointer a, gconstpointer b)
{
  const RequiredVersion *ver_a = a;
  const RequiredVersion *ver_b = b;

  return strcmp (ver_a->name, ver_b->name);
}

static gint
compare_package_keys (gconstpointer a, gconstpointer b)
{
  const Package *pkg_a = a;
  const Package *pkg_b = b;

  return strcmp (pkg_a->key, pkg_b->key);
}

static void
verify_package (Package *pkg)
{
  GSList *requires = NULL;
  GSList *conflicts = NULL;
  GSList *iter;
  GSList *requires_iter;
  GSList *conflicts_iter;  

  /* Be sure we have the required fields */

  if (pkg->key == NULL)
    {
      fprintf (stderr,
               "Internal pkg-config error, package with no key, please file a bug report\n");
      exit (1);
    }
  
  if (pkg->name == NULL)
    {
      verbose_error ("Package '%s' has no Name: field\n",
                     pkg->key);
      exit (1);
    }

  if (pkg->version == NULL)
    {
      verbose_error ("Package '%s' has no Version: field\n",
                     pkg->name);
      exit (1);
    }

  if (pkg->description == NULL)
    {
      verbose_error ("Package '%s' has no Description: field\n",
                     pkg->description);
      exit (1);
    }
  
  /* Make sure we have the right version for all requirements */

  iter = pkg->requires;

  while (iter != NULL)
    {
      Package *req = iter->data;
      RequiredVersion *ver = NULL;

      if (pkg->required_versions)
        ver = g_hash_table_lookup (pkg->required_versions,
                                   req->key);

      if (ver)
        {
          if (!version_test (ver->comparison, req->version, ver->version))
            {
              verbose_error ("Package '%s' requires '%s %s %s' but version of %s is %s\n",
                             pkg->name, req->key,
                             comparison_to_str (ver->comparison),
                             ver->version,
                             req->name,
                             req->version);

              exit (1);
            }
        }
                                   
      iter = g_slist_next (iter);
    }

  /* Make sure we didn't drag in any conflicts via Requires
   * (inefficient algorithm, who cares)
   */
  
  recursive_fill_list (pkg, get_requires, &requires);
  recursive_fill_list (pkg, get_conflicts, &conflicts);

  requires_iter = requires;
  while (requires_iter != NULL)
    {
      Package *req = requires_iter->data;
      
      conflicts_iter = conflicts;

      while (conflicts_iter != NULL)
        {
          RequiredVersion *ver = conflicts_iter->data;

          if (version_test (ver->comparison,
                            req->version,
                            ver->version))
            {
              verbose_error ("Version %s of %s creates a conflict.\n"
                             "(%s %s %s conflicts with %s %s)\n",
                             req->version, req->name,
                             ver->name,
                             comparison_to_str (ver->comparison),
                             ver->version,
                             ver->owner->name,
                             ver->owner->version);

              exit (1);
            }

          conflicts_iter = g_slist_next (conflicts_iter);
        }
      
      requires_iter = g_slist_next (requires_iter);
    }
  
  g_slist_free (requires);
  g_slist_free (conflicts);
}

static char*
get_merged (Package *pkg, GetListFunc func)
{
  GSList *list;
  GSList *dups_list = NULL;
  char *retval;
  
  recursive_fill_list (pkg, func, &dups_list);
  
  list = string_list_strip_duplicates (dups_list);

  g_slist_free (dups_list);
  
  retval = string_list_to_string (list);

  g_slist_free (list);
  
  return retval;
}

static char*
get_merged_from_back (Package *pkg, GetListFunc func)
{
  GSList *list;
  GSList *dups_list = NULL;
  char *retval;
  
  recursive_fill_list (pkg, func, &dups_list);
  
  list = string_list_strip_duplicates_from_back (dups_list);

  g_slist_free (dups_list);
  
  retval = string_list_to_string (list);

  g_slist_free (list);
  
  return retval;
}

static char*
get_multi_merged (GSList *pkgs, GetListFunc func)
{
  GSList *tmp;
  GSList *dups_list = NULL;
  GSList *list;
  char *retval;

  tmp = pkgs;
  while (tmp != NULL)
    {
      recursive_fill_list (tmp->data, func, &dups_list);  
      
      tmp = g_slist_next (tmp);
    }
  
  list = string_list_strip_duplicates (dups_list);

  g_slist_free (dups_list);
  
  retval = string_list_to_string (list);

  g_slist_free (list);
  
  return retval;
}

static char*
get_multi_merged_from_back (GSList *pkgs, GetListFunc func)
{
  GSList *tmp;
  GSList *dups_list = NULL;
  GSList *list;
  char *retval;

  tmp = pkgs;
  while (tmp != NULL)
    {
      recursive_fill_list (tmp->data, func, &dups_list);  
      
      tmp = g_slist_next (tmp);
    }
  
  list = string_list_strip_duplicates_from_back (dups_list);

  g_slist_free (dups_list);
  
  retval = string_list_to_string (list);

  g_slist_free (list);
  
  return retval;
}

char *
package_get_l_libs (Package *pkg)
{
  if (pkg->l_libs_merged == NULL)
    pkg->l_libs_merged = get_merged_from_back (pkg, get_l_libs);

  return pkg->l_libs_merged;
}

char *
packages_get_l_libs (GSList     *pkgs)
{
  return get_multi_merged_from_back (pkgs, get_l_libs);
}

char *
package_get_L_libs (Package *pkg)
{
  if (pkg->L_libs_merged == NULL)
    pkg->L_libs_merged = get_merged (pkg, get_L_libs);

  return pkg->L_libs_merged;

}

char *
packages_get_L_libs (GSList     *pkgs)
{
  return get_multi_merged (pkgs, get_L_libs);
}

char *
package_get_other_libs (Package *pkg)
{
  return g_strdup (pkg->other_libs);
}

char *
packages_get_other_libs (GSList   *pkgs)
{
  GSList *tmp;
  GString *str;
  char *retval;
  
  str = g_string_new ("");
  
  tmp = pkgs;
  while (tmp != NULL)
    {
      Package *pkg = tmp->data;

      if (pkg->other_libs)
        {
          g_string_append (str, pkg->other_libs);
          g_string_append (str, " ");
        }

      tmp = g_slist_next (tmp);
    }

  retval = str->str;
  g_string_free (str, FALSE);

  return retval;
}

char *
packages_get_all_libs (GSList *pkgs)
{
  char *l_libs;
  char *L_libs;
  char *other_libs;
  GString *str;
  char *retval;
  
  str = g_string_new ("");  

  other_libs = packages_get_other_libs (pkgs);
  L_libs = packages_get_L_libs (pkgs);
  l_libs = packages_get_l_libs (pkgs);

  if (other_libs)
    g_string_append (str, other_libs);
  
 if (L_libs)
    g_string_append (str, L_libs);
  
  if (l_libs)
    g_string_append (str, l_libs); 

  g_free (l_libs);
  g_free (L_libs);
  g_free (other_libs);

  retval = str->str;

  g_string_free (str, FALSE);

  return retval;
}

char *
package_get_I_cflags (Package *pkg)
{
  if (pkg->I_cflags_merged == NULL)
    pkg->I_cflags_merged = get_merged (pkg, get_I_cflags);

  return pkg->I_cflags_merged;
}

char *
packages_get_I_cflags (GSList     *pkgs)
{
  return get_multi_merged (pkgs, get_I_cflags);
}

char *
package_get_other_cflags (Package *pkg)
{
  return g_strdup (pkg->other_cflags);
}

char *
packages_get_other_cflags (GSList *pkgs)
{
  GSList *tmp;
  GString *str;
  char *retval;
  
  str = g_string_new ("");
  
  tmp = pkgs;
  while (tmp != NULL)
    {
      Package *pkg = tmp->data;

      if (pkg->other_cflags)
        {
          g_string_append (str, pkg->other_cflags);
          g_string_append (str, " ");
        }

      tmp = g_slist_next (tmp);
    }

  retval = str->str;
  g_string_free (str, FALSE);

  return retval;
}

char *
package_get_cflags (Package *pkg)
{

  g_assert_not_reached ();
  return NULL;
}

char *
packages_get_all_cflags (GSList     *pkgs)
{
  char *I_cflags;
  char *other_cflags;
  GString *str;
  char *retval;
  
  str = g_string_new ("");  

  other_cflags = packages_get_other_cflags (pkgs);
  I_cflags = packages_get_I_cflags (pkgs);

  if (other_cflags)
    g_string_append (str, other_cflags);
  
 if (I_cflags)
    g_string_append (str, I_cflags);

  g_free (I_cflags);
  g_free (other_cflags);

  retval = str->str;

  g_string_free (str, FALSE);

  return retval;
}


void
define_global_variable (const char *varname,
                        const char *varval)
{
  if (globals == NULL)
    globals = g_hash_table_new (g_str_hash, g_str_equal);

  if (g_hash_table_lookup (globals, varname))
    {
      verbose_error ("Variable '%s' defined twice globally\n", varname);
      exit (1);
    }
  
  g_hash_table_insert (globals, g_strdup (varname), g_strdup (varval));
      
  debug_spew ("Global variable definition '%s' = '%s'\n",
              varname, varval);
}

char *
package_get_var (Package *pkg,
                 const char *var)
{
  char *varval = NULL;

  if (globals)
    varval = g_hash_table_lookup (globals, var);
  
  if (varval == NULL && pkg->vars)
    varval = g_strdup (g_hash_table_lookup (pkg->vars, var));

  /* Magic "pcfiledir" variable */
  if (varval == NULL && pkg->pcfiledir && strcmp (var, "pcfiledir") == 0)
    varval = g_strdup (pkg->pcfiledir);

  return varval;
}

char *
packages_get_var (GSList     *pkgs,
                  const char *varname)
{
  GSList *tmp;
  GString *str;
  char *retval;
  
  str = g_string_new ("");
  
  tmp = pkgs;
  while (tmp != NULL)
    {
      Package *pkg = tmp->data;
      char *var;

      var = package_get_var (pkg, varname);
      
      if (var)
        {
          g_string_append (str, var);
          g_string_append_c (str, ' ');                
          g_free (var);
        }

      tmp = g_slist_next (tmp);
    }

  /* chop last space */
  str->str[str->len - 1] = '\0';
  retval = str->str;
  g_string_free (str, FALSE);

  return retval;
}



/* Stolen verbatim from rpm/lib/misc.c 
   RPM is Copyright (c) 1998 by Red Hat Software, Inc.,
   and may be distributed under the terms of the GPL and LGPL.
*/
/* compare alpha and numeric segments of two versions */
/* return 1: a is newer than b */
/*        0: a and b are the same version */
/*       -1: b is newer than a */
static int rpmvercmp(const char * a, const char * b) {
    char oldch1, oldch2;
    char * str1, * str2;
    char * one, * two;
    int rc;
    int isnum;
    
    /* easy comparison to see if versions are identical */
    if (!strcmp(a, b)) return 0;

    str1 = alloca(strlen(a) + 1);
    str2 = alloca(strlen(b) + 1);

    strcpy(str1, a);
    strcpy(str2, b);

    one = str1;
    two = str2;

    /* loop through each version segment of str1 and str2 and compare them */
    while (*one && *two) {
	while (*one && !isalnum(*one)) one++;
	while (*two && !isalnum(*two)) two++;

	str1 = one;
	str2 = two;

	/* grab first completely alpha or completely numeric segment */
	/* leave one and two pointing to the start of the alpha or numeric */
	/* segment and walk str1 and str2 to end of segment */
	if (isdigit(*str1)) {
	    while (*str1 && isdigit(*str1)) str1++;
	    while (*str2 && isdigit(*str2)) str2++;
	    isnum = 1;
	} else {
	    while (*str1 && isalpha(*str1)) str1++;
	    while (*str2 && isalpha(*str2)) str2++;
	    isnum = 0;
	}
		
	/* save character at the end of the alpha or numeric segment */
	/* so that they can be restored after the comparison */
	oldch1 = *str1;
	*str1 = '\0';
	oldch2 = *str2;
	*str2 = '\0';

	/* take care of the case where the two version segments are */
	/* different types: one numeric and one alpha */
	if (one == str1) return -1;	/* arbitrary */
	if (two == str2) return -1;

	if (isnum) {
	    /* this used to be done by converting the digit segments */
	    /* to ints using atoi() - it's changed because long  */
	    /* digit segments can overflow an int - this should fix that. */
	  
	    /* throw away any leading zeros - it's a number, right? */
	    while (*one == '0') one++;
	    while (*two == '0') two++;

	    /* whichever number has more digits wins */
	    if (strlen(one) > strlen(two)) return 1;
	    if (strlen(two) > strlen(one)) return -1;
	}

	/* strcmp will return which one is greater - even if the two */
	/* segments are alpha or if they are numeric.  don't return  */
	/* if they are equal because there might be more segments to */
	/* compare */
	rc = strcmp(one, two);
	if (rc) return rc;
	
	/* restore character that was replaced by null above */
	*str1 = oldch1;
	one = str1;
	*str2 = oldch2;
	two = str2;
    }

    /* this catches the case where all numeric and alpha segments have */
    /* compared identically but the segment sepparating characters were */
    /* different */
    if ((!*one) && (!*two)) return 0;

    /* whichever version still has characters left over wins */
    if (!*one) return -1; else return 1;
}

int
compare_versions (const char * a, const char *b)
{
  return rpmvercmp (a, b);
}

gboolean
version_test (ComparisonType comparison,
              const char *a,
              const char *b)
{
  switch (comparison)
    {
    case LESS_THAN:
      return compare_versions (a, b) < 0;
      break;

    case GREATER_THAN:
      return compare_versions (a, b) > 0;
      break;

    case LESS_THAN_EQUAL:
      return compare_versions (a, b) <= 0;
      break;

    case GREATER_THAN_EQUAL:
      return compare_versions (a, b) >= 0;
      break;

    case EQUAL:
      return compare_versions (a, b) == 0;
      break;

    case NOT_EQUAL:
      return compare_versions (a, b) != 0;
      break;

    case ALWAYS_MATCH:
      return TRUE;
      break;
      
    default:
      g_assert_not_reached ();
      break;
    }

  return FALSE;
}

const char *
comparison_to_str (ComparisonType comparison)
{
  switch (comparison)
    {
    case LESS_THAN:
      return "<";
      break;

    case GREATER_THAN:
      return ">";
      break;

    case LESS_THAN_EQUAL:
      return "<=";
      break;

    case GREATER_THAN_EQUAL:
      return ">=";
      break;

    case EQUAL:
      return "=";
      break;

    case NOT_EQUAL:
      return "!=";
      break;

    case ALWAYS_MATCH:
      return "(any)";
      break;
      
    default:
      g_assert_not_reached ();
      break;
    }

  return "???";
}

static void
packages_foreach (gpointer key, gpointer value, gpointer data)
{
  Package *pkg = get_package (key);

  if (pkg != NULL)
    {
      printf ("%s \t\t%s - %s\n",
              pkg->key, pkg->name, pkg->description);
    }
}

void
print_package_list (void)
{
  g_hash_table_foreach (locations, packages_foreach, NULL);
}

