
/******************************************************************************
* MODULE     : edit_interface.cpp
* DESCRIPTION: interface between the editor and the window manager
* COPYRIGHT  : (C) 1999  Joris van der Hoeven
*******************************************************************************
* This software falls under the GNU general public license and comes WITHOUT
* ANY WARRANTY WHATSOEVER. See the file $TEXMACS_PATH/LICENSE for more details.
* If you don't have this file, write to the Free Software Foundation, Inc.,
* 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
******************************************************************************/

#include "Interface/edit_interface.hpp"
#include "file.hpp"
#include "convert.hpp"
#include "connect.hpp"
#include "server.hpp"
#include "tm_buffer.hpp"

extern void (*env_next_prog)(void);
extern void clear_rectangles (ps_device dev, rectangles l);
extern void selection_correct (tree t, path i1, path i2, path& o1, path& o2);

/*static*/ string
LANGUAGE (string mode) {
  if (mode == "text") return TEXT_LANGUAGE;
  if (mode == "math") return MATH_LANGUAGE;
  if (mode == "prog") return PROG_LANGUAGE;
  cerr << "Mode = " << mode << "\n";
  fatal_error ("invalid mode", "the_language", "edit_interface.cpp");
  return TEXT_LANGUAGE;
}

/******************************************************************************
* Main edit_interface routines
******************************************************************************/

edit_interface_rep::edit_interface_rep ():
  env_change (0),
  last_change (texmacs_time()), last_update (last_change-1),
  con_status (CONNECTION_DEAD),
  full_screen (false), got_focus (false),
  sh_s (""), sh_len (0),
  popup_win (NULL),
  sfactor (sv->get_default_shrinking_factor ()),
  pixel (sfactor*PIXEL), copy_always (),
  last_click (0), dragging (false),
  made_selection (false), table_selection (false),
  oc (0, 0)
{
  input_mode  = INPUT_NORMAL;
}

edit_interface_rep::~edit_interface_rep () {}

edit_interface_rep::operator tree () {
  return tuple ("edit", as_string (get_name ()));
}

void
edit_interface_rep::suspend () {
  got_focus= false;
  notify_change (THE_FOCUS);
}

void
edit_interface_rep::resume () {
  got_focus= true;
  SERVER (menu_main ("(horizontal (link texmacs-menu))"));
  SERVER (menu_icons (0, "(horizontal (link texmacs-main-icons))"));
  SERVER (menu_icons (1, "(horizontal (link texmacs-context-icons))"));
  SERVER (menu_icons (2, "(horizontal (link texmacs-extra-icons))"));
  notify_change (THE_FOCUS + THE_EXTENTS);
}

display
edit_interface_rep::get_display () {
  return dis;
}

widget
edit_interface_rep::get_widget () {
  return widget (this);
}

/******************************************************************************
* Active sessions
******************************************************************************/

void
edit_interface_rep::update_connection () {
  // cout << "et= " << et << "\n";
  // cout << "tp= " << tp << "\n";
  con_name   = get_env_string (PROG_LANGUAGE);
  con_session= get_env_string (THIS_SESSION);
  con_status = connection_status (con_name, con_session);
  // cout << "Name   : " << con_name << "\n";
  // cout << "Session: " << con_session << "\n";
  // cout << "Status : " << con_status << "\n";
}

void
edit_interface_rep::connect () {
  update_connection ();
  string s= connection_start (con_name, con_session);
  if (s != "ok") set_message (s, "connect#" * con_name);
  else set_message (con_name*"#is running...", "session#`"*con_session*"'");
  con_status = connection_status (con_name, con_session);
  if (con_status == WAITING_FOR_INPUT) start_input ();
  else if (con_status == WAITING_FOR_OUTPUT) start_output ();
}

void
edit_interface_rep::process_extern_input () {
  if (con_status == WAITING_FOR_OUTPUT) {
    update_connection ();
    if (con_status != WAITING_FOR_OUTPUT) return;
    tree doc= connection_read (con_name, con_session, "output");
    if (doc != "") {
      insert_tree (doc);
      set_message (con_name * "#is running...",
		   "session#`" * con_session * "'");
    }
    doc= connection_read (con_name, con_session, "error");
    if (doc != "") {
      insert_tree (compound ("errput", doc));
      set_message (con_name * "#is running...",
		   "session#`" * con_session * "'");
    }
    con_status= connection_status (con_name, con_session);
    if (con_status == CONNECTION_DEAD) {
      start_input ();
      set_message (con_name * "#has completed its task",
		   "session#`" * con_session * "'");
    }
    else if (con_status == WAITING_FOR_INPUT)
      start_input ();
  }
}

void
edit_interface_rep::feed_input (tree t) {
  update_connection ();
  if (con_status == WAITING_FOR_INPUT) {
    connection_write (con_name, con_session, t);
    con_status= WAITING_FOR_OUTPUT;
  }
  else if (con_status == CONNECTION_DEAD) {
    string s= connection_start (con_name, con_session, true);
    if (s != "ok") {
      set_message (s, "connect#" * con_name);
      start_input ();
    }
    else {
      connection_write (con_name, con_session, t);
      con_status= WAITING_FOR_OUTPUT;
      session_message ("Warning: " * con_name * "#has been restarted",
		       "session#`" * con_session * "'");
    }
  }
}

bool
edit_interface_rep::busy_connection () {
  update_connection ();
  return (con_status == WAITING_FOR_OUTPUT);
}

void
edit_interface_rep::interrupt_connection () {
  update_connection ();
  if (con_status == WAITING_FOR_OUTPUT) {
    connection_interrupt (con_name, con_session);
    update_connection ();
  }
}

void
edit_interface_rep::stop_connection () {
  update_connection ();
  if (con_status != CONNECTION_DEAD) {
    connection_stop (con_name, con_session);
    update_connection ();
  }
}

/******************************************************************************
* Some routines for dealing with shrinked coordinates
******************************************************************************/

void
edit_interface_rep::set_shrinking_factor (int sf) {
  if (sfactor != sf) {
    sfactor= sf;
    pixel  = sf*PIXEL;
    init_env (SFACTOR, as_string (sf));
    notify_change (THE_ENVIRONMENT);
    notify_change (THE_AUTOMATIC_SIZE);
  }
}

void
edit_interface_rep::invalidate (SI x1, SI y1, SI x2, SI y2) {
  this << emit_invalidate ((x1-sfactor+1)/sfactor, (y1-sfactor+1)/sfactor,
			   (x2+sfactor-1)/sfactor, (y2+sfactor-1)/sfactor);
}

void
edit_interface_rep::invalidate (rectangles rs) {
  while (!nil (rs)) {
    invalidate (rs->item->x1-pixel, rs->item->y1-pixel,
		rs->item->x2+pixel, rs->item->y2+pixel);
    rs= rs->next;
  }
}

void
edit_interface_rep::get_visible (SI& x1, SI& y1, SI& x2, SI& y2) {
  SERVER (get_visible (x1, y1, x2, y2));
  x1*=sfactor; y1*=sfactor; x2*=sfactor; y2*=sfactor;
}

SI
edit_interface_rep::get_window_height () {
  SI x1, y1, x2, y2;
  get_visible (x1, y1, x2, y2);
  return (y2-y1);
}

void
edit_interface_rep::scroll_to (SI x, SI y) {
  SERVER (scroll_to (x/sfactor, y/sfactor));
}

void
edit_interface_rep::set_extents (SI x1, SI y1, SI x2, SI y2) {
  SERVER (set_extents ((x1-sfactor+1)/sfactor, (y1-sfactor+1)/sfactor,
		       (x2+sfactor-1)/sfactor, (y2+sfactor-1)/sfactor));
}

/******************************************************************************
* repainting the window
******************************************************************************/

extern int nr_painted;

void
edit_interface_rep::draw_text (repaint_event ev) {
  ps_device dev= win->window_to_shadow (
    ev->x1/sfactor, ev->y1/sfactor,
    ev->x2/sfactor, ev->y2/sfactor);
  dev->set_shrinking_factor (sfactor);
  string bg= get_init_string (BACKGROUND_COLOR);
  dev->set_background (dis->get_color (bg));
  rectangle r (
    ev->x1+ dev->ox, ev->y1+ dev->oy,
    ev->x2+ dev->ox, ev->y2+ dev->oy);
  clear_rectangles (dev, rectangles (r));
  draw_surround (dev, ev->x1, ev->y1, ev->x2, ev->y2);
  //win->set_shrinking_factor (sfactor);
  //win->set_background (dis->red);
  //clear_rectangles (win, rectangles (r));  
  //win->set_shrinking_factor (1);
  //dev->set_background (dis->get_color (bg));

  draw_cursor (dev);
  // rectangles l= translate (copy_always, dev->ox, dev->oy);
  rectangles l= copy_always;
  while (!nil (l)) {
    rectangle r (l->item);
    dev->apply_shadow (r->x1, r->y1, r->x2, r->y2);
    l= l->next;
  }
  // rectangles l;
  nr_painted=0;
  bool tp_found= false;
  dev->set_background (dis->get_color (bg));
  eb->redraw (dev, eb->find_box_path (tp, tp_found), l);
  draw_cursor (dev);
  draw_selection (dev);
  if (dev->check_event (EVENT_STATUS)) {
    ev->stop= true;
    l= l & rectangles (r);
    simplify (l);
  }
  else l= rectangles (r);
  copy_always= translate (copy_always, dev->ox, dev->oy);
  while (!nil (copy_always)) {
    l= rectangles (copy_always->item, l);
    copy_always= copy_always->next;
  }

  dev->set_shrinking_factor (1);
  while (!nil(l)) {
    SI x1= (l->item->x1)/sfactor - dev->ox - PIXEL;
    SI y1= (l->item->y1)/sfactor - dev->oy - PIXEL;
    SI x2= (l->item->x2)/sfactor - dev->ox + PIXEL;
    SI y2= (l->item->y2)/sfactor - dev->oy + PIXEL;
    dev->outer_round (x1, y1, x2, y2);
    win->shadow_to_window (x1, y1, x2, y2);
    l= l->next;
  }
}

void
edit_interface_rep::draw_env (ps_device dev) {
  if (!full_screen) {
    rectangles rs= env_rects;
    while (!nil (rs)) {
      dev->set_color (dis->rgb (0, 255, 255));
      dev->fill (rs->item->x1, rs->item->y1, rs->item->x2, rs->item->y2);
      rs= rs->next;
    }
  }
}

void
edit_interface_rep::draw_cursor (ps_device dev) {
  if (got_focus || full_screen) {
    draw_env (dev);
    cursor cu= copy (the_cursor());
    cu->y1 -= 2*pixel; cu->y2 += 2*pixel;
    SI x1= cu->ox + ((SI) (cu->y1 * cu->slope)), y1= cu->oy + cu->y1;
    SI x2= cu->ox + ((SI) (cu->y2 * cu->slope)), y2= cu->oy + cu->y2;
    dev->set_line_style (pixel);
    string mode= get_env_string (MODE);
    string family, series;
    if (mode == "text") {
      family= get_env_string (TEXT_FAMILY);
      series= get_env_string (TEXT_SERIES);
    }
    else if (mode == "math") {
      family= get_env_string (MATH_FAMILY);
      series= get_env_string (MATH_SERIES);
    }
    else if (mode == "prog") {
      family= get_env_string (PROG_FAMILY);
      series= get_env_string (PROG_SERIES);
    }
    if (cu->valid) {
      if (mode == "math")
	dev->set_color (dis->rgb (192, 0, 255));
      else dev->set_color (dis->red);
    }
    else dev->set_color (dis->green);
    SI lserif= (series=="bold"? 2*pixel: pixel), rserif= pixel;
    if (family == "ss") lserif= rserif= 0;
    dev->line (x1-lserif, y1, x1+rserif, y1);
    if (y1<=y2-pixel) {
      dev->line (x1, y1, x2, y2-pixel);
      if (series == "bold") dev->line (x1-pixel, y1, x2-pixel, y2-pixel);
      dev->line (x2-lserif, y2-pixel, x2+rserif, y2-pixel);
    }
  }
}

void
edit_interface_rep::draw_surround (ps_device dev, SI X1, SI Y1, SI X2, SI Y2) {
  dev->set_background (dis->light_grey);
  string medium= get_init_string (PAGE_MEDIUM);
  if ((medium == "papyrus") || (medium == "paper"))
    dev->clear (max (eb->x2, X1), Y1, X2, min (eb->y2+ 2*pixel, Y2));
  else if (medium == "paper")
    dev->clear (X1, Y1, X2, min (eb->y1, Y2));
}

void
edit_interface_rep::draw_context (repaint_event ev) {
  int i;
  win->set_color (dis->light_grey);
  win->set_line_style (pixel);
  for (i=1; i<N(eb[0]); i++) {
    SI y= eb->sy(0)+ eb[0]->sy2(i);
    if ((y >= ev->y1) && (y < ev->y2))
      win->line (ev->x1, y, ev->x2, y);
  }
  draw_surround (win, ev->x1, ev->y1, ev->x2, ev->y2);
}

void
edit_interface_rep::draw_selection (ps_device dev) {
  if (made_selection) {
    rectangles rs= selection_rects;
    while (!nil (rs)) {
      dev->set_color (table_selection? dis->rgb (192, 0, 255): dis->red);
      dev->fill (rs->item->x1, rs->item->y1, rs->item->x2, rs->item->y2);
      rs= rs->next;
    }
  }
}

void
edit_interface_rep::handle_clear (clear_event ev) {
  SI X1= ev->x1 * sfactor, Y1= ev->y1 * sfactor;
  SI X2= ev->x2 * sfactor, Y2= ev->y2 * sfactor;
  win->set_shrinking_factor (sfactor);
  string bg= get_init_string (BACKGROUND_COLOR);
  win->set_background (dis->get_color (bg));
  win->clear (max (eb->x1, X1), max (eb->y1, Y1),
	      min (eb->x2, X2), min (eb->y2, Y2));
  draw_surround (win, X1, Y1, X2, Y2);
  win->set_shrinking_factor (1);
}

void
edit_interface_rep::handle_repaint (repaint_event ev) {
  if (env_change != 0)
    fatal_error ("Invalid situation", "edit_interface_rep::handle_repaint");

  // cout << "Repainting\n";
  // Repaint slightly more in order to hide trace of moving cursor
  SI extra= 3 * get_init_int(FONT_BASE_SIZE) * PIXEL / (2*sfactor);
  event sev= ::emit_repaint
    ((ev->x1-extra)*sfactor, (ev->y1-extra)*sfactor,
     (ev->x2+extra)*sfactor, (ev->y2+extra)*sfactor, ev->stop);
  draw_text (sev);
  win->set_shrinking_factor (sfactor);
  draw_context (sev);
  draw_cursor (win);
  draw_selection (win);
  win->set_shrinking_factor (1);
  if (last_change>last_update) last_change= texmacs_time ();
  // cout << "Repainted\n";
}

/******************************************************************************
* handling changes
******************************************************************************/

void
edit_interface_rep::notify_change (int change) {
  env_change= env_change | change;
}

bool
edit_interface_rep::has_changed (int question) {
  return (env_change & question) != 0;
}

void
edit_interface_rep::cursor_visible () {
  cursor cu= copy (the_cursor ());
  cu->y1 -= 2*pixel; cu->y2 += 2*pixel;
  SI x1, y1, x2, y2;
  get_visible (x1, y1, x2, y2);
  if ((cu->ox+ ((SI) (cu->y1 * cu->slope)) <  x1) ||
      (cu->ox+ ((SI) (cu->y2 * cu->slope)) >= x2) ||
      (cu->oy+ cu->y1 <  y1) ||
      (cu->oy+ cu->y2 >= y2))
    {
      scroll_to (cu->ox- ((x2-x1)>>1), cu->oy+ ((y2-y1)>>1));
      this << emit_invalidate_all ();
    }
}

void
edit_interface_rep::selection_visible () {
  SI x1, y1, x2, y2;
  get_visible (x1, y1, x2, y2);
  if ((x2-x1 <= 80*pixel) || (y2-y1 <= 80*pixel)) return;

  bool scroll_x=
    (end_x <  x1+20*pixel) ||
    (end_x >= x2-20*pixel);
  bool scroll_y=
    (end_y <  y1+20*pixel) ||
    (end_y >= y2-20*pixel);
  SI new_x= x1;
  if (scroll_x) new_x= end_x- ((x2-x1)>>1);
  SI new_y= y2;
  if (scroll_y) new_y= end_y+ ((y2-y1)>>1);

  if (scroll_x || scroll_y) {
    scroll_to (new_x, new_y);
    this << emit_invalidate_all ();
    SI X1, Y1, X2, Y2;
    get_visible (X1, Y1, X2, Y2);
    end_x += X1- x1;
    end_y += Y1- y1;
  }
}

void
edit_interface_rep::apply_changes () {
  if (env_change == 0) {
    if ((last_update < last_change) &&
	(texmacs_time() >= (last_change + (1000/6))) &&
	win->repainted() &&
	(!win->check_event (EVENT_STATUS)) &&
	got_focus)
      {
	call ("lazy-in-mode-force");
	SERVER (menu_main ("(horizontal (link texmacs-menu))"));
	SERVER (menu_icons (0, "(horizontal (link texmacs-main-icons))"));
	SERVER (menu_icons (1, "(horizontal (link texmacs-context-icons))"));
	SERVER (menu_icons (2, "(horizontal (link texmacs-extra-icons))"));
	set_footer ();
	update_connection ();
	last_update= last_change;
      }
    return;
  }

  // cout << "Applying changes " << env_change << " to " << get_name() << "\n";

  // cout << "Handling automatic resizing\n";
  if (env_change & THE_AUTOMATIC_SIZE) {
    if (get_init_string (PAGE_MEDIUM) == "automatic") {
      if (!attached())
	fatal_error ("Window not attached",
		     "edit_interface_rep::apply_changes");
      SI wx, wy;
      win->get_size (wx, wy);
      init_env (PAGE_WIDTH, as_string ((wx-20*PIXEL)*sfactor) * "unit");
      init_env (PAGE_HEIGHT, as_string (wy*sfactor) * "unit");
      notify_change (THE_ENVIRONMENT);
    }
  }

  // cout << "Handling selection\n";
  if (env_change & (THE_TREE+THE_ENVIRONMENT+THE_SELECTION)) {
    if (made_selection) invalidate (selection_rects);
  }

  // cout << "Handling environment\n";
  if (env_change & THE_ENVIRONMENT) typeset_preamble ();

  // cout << "Handling tree\n";
  if (env_change & (THE_TREE+THE_ENVIRONMENT)) {
    typeset_invalidate_env ();
    if (input_mode == INPUT_NORMAL) {
      made_selection= false;
      set_selection (tp, tp);
    }
    selection_rects= rectangles ();
    SI x1, y1, x2, y2;
    typeset (x1, y1, x2, y2);
    invalidate (x1- 2*pixel, y1- 2*pixel, x2+ 2*pixel, y2+ 2*pixel);
    // check_data_integrety ();
    the_ghost_cursor()= eb->find_check_cursor (tp);
  }

  // cout << "Handling extents\n";
  if (env_change & (THE_TREE+THE_ENVIRONMENT+THE_EXTENTS))
    set_extents (eb->x1, eb->y1, eb->x2, eb->y2);

  // cout << "Cursor\n";
  if (env_change & (THE_TREE+THE_ENVIRONMENT+THE_EXTENTS+
		    THE_CURSOR+THE_FOCUS)) {
    SI /*P1= pixel,*/ P2= 2*pixel, P3= 3*pixel;
    int THE_CURSOR_BAK= env_change & THE_CURSOR;
    go_to_here ();
    env_change= (env_change & (~THE_CURSOR)) | THE_CURSOR_BAK;
    if (env_change & (THE_TREE+THE_ENVIRONMENT+THE_EXTENTS+THE_CURSOR))
      cursor_visible ();

    cursor& cu= the_cursor();
    rectangle ocr (oc->ox+ ((SI) (oc->y1*oc->slope))- P3, oc->oy+ oc->y1- P3,
		   oc->ox+ ((SI) (oc->y2*oc->slope))+ P2, oc->oy+ oc->y2+ P3);
    copy_always= rectangles (ocr, copy_always);
    invalidate (ocr->x1, ocr->y1, ocr->x2, ocr->y2);
    rectangle ncr (cu->ox+ ((SI) (cu->y1*cu->slope))- P3, cu->oy+ cu->y1- P3,
		   cu->ox+ ((SI) (cu->y2*cu->slope))+ P2, cu->oy+ cu->y2+ P3);
    invalidate (ncr->x1, ncr->y1, ncr->x2, ncr->y2);
    copy_always= rectangles (ncr, copy_always);
    oc= copy (cu);

    rectangles old_rects= env_rects;
    env_rects= rectangles ();
    compute_env_rects (path_up (tp), env_rects, true);
    if (env_rects != old_rects) {
      invalidate (old_rects);
      invalidate (env_rects);
    }
    else if (env_change & THE_FOCUS) invalidate (env_rects);
  }

  // cout << "Handling selection\n";
  if (env_change & THE_SELECTION) {
    made_selection= selection_active_any ();
    if (made_selection) {
      table_selection= selection_active_table ();
      selection sel; selection_get (sel);
      /*
      selection_rects=
	simplify (::correct (thicken (sel->rs, pixel, pixel) - sel->rs));
      */
      selection_rects= simplify (::correct (thicken (sel->rs, pixel, 3*pixel) -
					    thicken (sel->rs, 0, 2*pixel)));
      invalidate (selection_rects);
    }
  }

  // cout << "Handling environment changes\n";
  if (env_change & THE_ENVIRONMENT)
    this << emit_invalidate_all ();

  // cout << "Applied changes\n";
  env_change= 0;
  last_change= texmacs_time ();
  last_update= last_change-1;
}

/******************************************************************************
* miscellaneous routines
******************************************************************************/

bool
edit_interface_rep::kbd_get_command (string which, string& help, command& c) {
  return sv->kbd_get_command (which, help, c);
}

void
edit_interface_rep::full_screen_mode (bool flag) {
  full_screen= flag;
}

void
edit_interface_rep::compute_env_rects (path p, rectangles& rs, bool recurse) {
  p= path_up (p);
  if (nil (p)) return;
  tree st= subtree (et, p);
  if (is_atomic (st) || is_document (st) || is_concat (st) ||
      is_func (st, TABLE) || is_func (st, SUB_TABLE) ||
      is_func (st, ROW) || is_func (st, CELL) || is_func (st, TABLE_FORMAT))
    compute_env_rects (p, rs, recurse);
  else {
    bool right;
    path p1= p * 0, p2= p * 1, q1, q2;
    if (is_script (subtree (et, p), right)) {
      p1= start (et, p * 0);
      p2= end   (et, p * 0);
    }
    selection_correct (et, p1, p2, q1, q2);
    selection sel= eb->find_check_selection (q1, q2);
    rs << simplify (::correct (thicken (sel->rs, pixel, 3*pixel) -
			       thicken (sel->rs, 0, 2*pixel)));
    if (recurse) compute_env_rects (p, rs, recurse);
  }
}

void
edit_interface_rep::before_menu_action () {
  buf->mark_undo_block ();
  set_input_normal ();
}

void
edit_interface_rep::after_menu_action () {
  notify_change (THE_DECORATIONS);
}

/******************************************************************************
* event handlers
******************************************************************************/

void
edit_interface_rep::handle_get_size (get_size_event ev) {
  dis->get_extents (ev->w, ev->h);
}

void
edit_interface_rep::handle_attach_window (attach_window_event ev) {
  basic_widget_rep::handle_attach_window (ev);
}

void
edit_interface_rep::handle_resize (resize_event ev) { (void) ev;
  if (get_init_string (PAGE_MEDIUM) == "automatic")
    notify_change (THE_AUTOMATIC_SIZE);
  notify_change (THE_TREE);
}

void
edit_interface_rep::handle_set_integer (set_integer_event ev) {
  if (ev->which == "shrinking factor")
    set_shrinking_factor (ev->i);
  else a[0]->a[0] << ev;
}
