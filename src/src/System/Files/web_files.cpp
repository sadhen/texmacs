
/******************************************************************************
* MODULE     : web_files.cpp
* DESCRIPTION: file handling via the web
* COPYRIGHT  : (C) 1999  Joris van der Hoeven
*******************************************************************************
* This software falls under the GNU general public license and comes WITHOUT
* ANY WARRANTY WHATSOEVER. See the file $TEXMACS_PATH/LICENSE for more details.
* If you don't have this file, write to the Free Software Foundation, Inc.,
* 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
******************************************************************************/

#include "file.hpp"
#include "web_files.hpp"
#include "sys_utils.hpp"
#include "analyze.hpp"
#include "hashmap.hpp"

#define MAX_CACHED 25
static int web_nr=0;
static array<tree> web_cache (MAX_CACHED);
static hashmap<tree,tree> web_cache_resolve ("");

/******************************************************************************
* Caching
******************************************************************************/

static url
get_cache (url name) {
  if (web_cache_resolve->contains (name->t)) {
    int i, j;
    tree tmp= web_cache_resolve [name->t];
    for (i=0; i<MAX_CACHED; i++)
      if (web_cache[i] == name->t) {
	// cout << name << " in cache as " << tmp << " at " << i << "\n";
	for (j=i; ((j+1) % MAX_CACHED) != web_nr; j= (j+1) % MAX_CACHED)
	  web_cache[j]= web_cache[(j+1) % MAX_CACHED];
	web_cache[j]= name->t;
	break;
      }
    return as_url (tmp); // url_system (tmp);
  }
  return url_none ();
}

static url
set_cache (url name, url tmp) {
  web_cache_resolve->reset (web_cache [web_nr]);
  web_cache [web_nr]= name->t;
  web_cache_resolve (name->t)= tmp->t;
  web_nr= (web_nr+1) % MAX_CACHED;
  return tmp;
}

/******************************************************************************
* Web files
******************************************************************************/

url
get_from_web (url name) {
  if (!is_rooted_web (name)) return url_none ();
  url res= get_cache (name);
  if (!is_none (res)) return (res);

  string test= var_eval_system ("which wget");
  if (!ends (test, "wget")) return url_none ();
  url tmp= url_temp ();
  string cmd= "wget --header='User-Agent: TeXmacs-" TEXMACS_VERSION "' -q";
  cmd << " -O " << concretize (tmp) << " " << as_string (name);
  // cout << cmd << "\n";
  system (cmd);
  // cout << "got " << name << " as " << tmp << "\n";

  if (var_eval_system ("cat " * concretize (tmp) * " 2> /dev/null") == "") {
    remove (tmp);
    return url_none ();
  }
  else return set_cache (name, tmp);
}

url
get_from_ramdisc (url u) {
  if (!is_ramdisc (u)) return url_none ();
  url res= get_cache (u);
  if (!is_none (res)) return (res);
  url tmp= url_temp ();
  save_string (tmp, u[1][2]->t->label);
  return set_cache (u, tmp);
}
