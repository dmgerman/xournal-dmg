#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <signal.h>
#include <memory.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <gtk/gtk.h>
#include <libgnomecanvas/libgnomecanvas.h>
#include <zlib.h>
#include <math.h>
#include <gdk/gdkx.h>
#include <X11/Xlib.h>

#include "xournal.h"
#include "xo-interface.h"
#include "xo-support.h"
#include "xo-callbacks.h"
#include "xo-misc.h"
#include "xo-file.h"

const char *tool_names[NUM_STROKE_TOOLS] = {"pen", "eraser", "highlighter"};
const char *color_names[COLOR_MAX] = {"black", "blue", "red", "green",
   "gray", "lightblue", "lightgreen", "magenta", "orange", "yellow", "white"};
const char *bgtype_names[3] = {"solid", "pixmap", "pdf"};
const char *bgcolor_names[COLOR_MAX] = {"", "blue", "pink", "green",
   "", "", "", "", "orange", "yellow", "white"};
const char *bgstyle_names[4] = {"plain", "lined", "ruled", "graph"};
const char *file_domain_names[3] = {"absolute", "attach", "clone"};

// creates a new empty journal

void new_journal(void)
{
  journal.npages = 1;
  journal.pages = g_list_append(NULL, new_page(&ui.default_page));
  journal.last_attach_no = 0;
  ui.pageno = 0;
  ui.layerno = 0;
  ui.cur_page = (struct Page *) journal.pages->data;
  ui.cur_layer = (struct Layer *) ui.cur_page->layers->data;
  ui.saved = TRUE;
  ui.filename = NULL;
  update_file_name(NULL);
}

// check attachment names

void chk_attach_names(void)
{
  GList *list;
  struct Background *bg;
  
  for (list = journal.pages; list!=NULL; list = list->next) {
    bg = ((struct Page *)list->data)->bg;
    if (bg->type == BG_SOLID || bg->file_domain != DOMAIN_ATTACH ||
        bg->filename->s != NULL) continue;
    bg->filename->s = g_strdup_printf("bg_%d.png", ++journal.last_attach_no);
  }
}

// saves the journal to a file: returns true on success, false on error

gboolean save_journal(const char *filename)
{
  gzFile f;
  struct Page *pg, *tmppg;
  struct Layer *layer;
  struct Item *item;
  int i, is_clone;
  char *tmpfn;
  gchar *pdfbuf;
  gsize pdflen;
  gboolean success;
  FILE *tmpf;
  GList *pagelist, *layerlist, *itemlist, *list;
  GtkWidget *dialog;
  
  f = gzopen(filename, "w");
  if (f==NULL) return FALSE;
  chk_attach_names();
  
  gzprintf(f, "<?xml version=\"1.0\" standalone=\"no\"?>\n"
     "<title>Xournal document - see http://math.mit.edu/~auroux/software/xournal/</title>\n"
     "<xournal version=\"" VERSION "\"/>\n");
  for (pagelist = journal.pages; pagelist!=NULL; pagelist = pagelist->next) {
    pg = (struct Page *)pagelist->data;
    gzprintf(f, "<page width=\"%.2f\" height=\"%.2f\">\n", pg->width, pg->height);
    gzprintf(f, "<background type=\"%s\" ", bgtype_names[pg->bg->type]); 
    if (pg->bg->type == BG_SOLID) {
      gzputs(f, "color=\"");
      if (pg->bg->color_no >= 0) gzputs(f, bgcolor_names[pg->bg->color_no]);
      else gzprintf(f, "#%08x", pg->bg->color_rgba);
      gzprintf(f, "\" style=\"%s\" ", bgstyle_names[pg->bg->ruling]);
    }
    else if (pg->bg->type == BG_PIXMAP) {
      is_clone = -1;
      for (list = journal.pages, i = 0; list!=pagelist; list = list->next, i++) {
        tmppg = (struct Page *)list->data;
        if (tmppg->bg->type == BG_PIXMAP && 
            tmppg->bg->pixbuf == pg->bg->pixbuf &&
            tmppg->bg->filename == pg->bg->filename)
          { is_clone = i; break; }
      }
      if (is_clone >= 0)
        gzprintf(f, "domain=\"clone\" filename=\"%d\" ", is_clone);
      else {
        if (pg->bg->file_domain == DOMAIN_ATTACH) {
          tmpfn = g_strdup_printf("%s.%s", filename, pg->bg->filename->s);
          if (!gdk_pixbuf_save(pg->bg->pixbuf, tmpfn, "png", NULL, NULL)) {
            dialog = gtk_message_dialog_new(GTK_WINDOW(winMain), GTK_DIALOG_MODAL,
              GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, 
              "Could not write background '%s'. Continuing anyway.", tmpfn);
            gtk_dialog_run(GTK_DIALOG(dialog));
            gtk_widget_destroy(dialog);
          }
          g_free(tmpfn);
        }
        gzprintf(f, "domain=\"%s\" filename=\"%s\" ", 
          file_domain_names[pg->bg->file_domain], pg->bg->filename->s);
      }
    }
    else if (pg->bg->type == BG_PDF) {
      is_clone = 0;
      for (list = journal.pages; list!=pagelist; list = list->next) {
        tmppg = (struct Page *)list->data;
        if (tmppg->bg->type == BG_PDF) { is_clone = 1; break; }
      }
      if (!is_clone) {
        if (pg->bg->file_domain == DOMAIN_ATTACH) {
          tmpfn = g_strdup_printf("%s.%s", filename, pg->bg->filename->s);
          success = FALSE;
          if (bgpdf.status != STATUS_NOT_INIT &&
              g_file_get_contents(bgpdf.tmpfile_copy, &pdfbuf, &pdflen, NULL))
          {
            tmpf = fopen(tmpfn, "w");
            if (tmpf != NULL && fwrite(pdfbuf, 1, pdflen, tmpf) == pdflen)
              success = TRUE;
            fclose(tmpf);
          }
          if (!success) {
            dialog = gtk_message_dialog_new(GTK_WINDOW(winMain), GTK_DIALOG_MODAL,
              GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, 
              "Could not write background '%s'. Continuing anyway.", tmpfn);
            gtk_dialog_run(GTK_DIALOG(dialog));
            gtk_widget_destroy(dialog);
          }
          g_free(tmpfn);
        }
        gzprintf(f, "domain=\"%s\" filename=\"%s\" ", 
          file_domain_names[pg->bg->file_domain], pg->bg->filename->s);
      }
      gzprintf(f, "pageno=\"%d\" ", pg->bg->file_page_seq);
    }
    gzprintf(f, "/>\n");
    for (layerlist = pg->layers; layerlist!=NULL; layerlist = layerlist->next) {
      layer = (struct Layer *)layerlist->data;
      gzprintf(f, "<layer>\n");
      for (itemlist = layer->items; itemlist!=NULL; itemlist = itemlist->next) {
        item = (struct Item *)itemlist->data;
        if (item->type == ITEM_STROKE) {
          gzprintf(f, "<stroke tool=\"%s\" color=\"", 
                          tool_names[item->brush.tool_type]);
          if (item->brush.color_no >= 0)
            gzputs(f, color_names[item->brush.color_no]);
          else
            gzprintf(f, "#%08x", item->brush.color_rgba);
          gzprintf(f, "\" width=\"%.2f\">\n", item->brush.thickness);
          for (i=0;i<2*item->path->num_points;i++)
            gzprintf(f, "%.2f ", item->path->coords[i]);
          gzprintf(f, "\n</stroke>\n");
        }
      }
      gzprintf(f, "</layer>\n");
    }
    gzprintf(f, "</page>\n");
  }
  gzclose(f);
  return TRUE;
}

// closes a journal: returns true on success, false on abort

gboolean close_journal(void)
{
  if (!ok_to_close()) return FALSE;
  
  // free everything...
  reset_selection();
  clear_redo_stack();
  clear_undo_stack();

  shutdown_bgpdf();
  delete_journal(&journal);
  
  return TRUE;
  /* note: various members of ui and journal are now in invalid states,
     use new_journal() to reinitialize them */
}

// the XML parser functions for open_journal()

struct Journal tmpJournal;
struct Page *tmpPage;
struct Layer *tmpLayer;
struct Item *tmpItem;
char *tmpFilename;
struct Background *tmpBg_pdf;

GError *xoj_invalid(void)
{
  return g_error_new(G_MARKUP_ERROR, G_MARKUP_ERROR_INVALID_CONTENT, "Invalid file contents");
}

void xoj_parser_start_element(GMarkupParseContext *context,
   const gchar *element_name, const gchar **attribute_names, 
   const gchar **attribute_values, gpointer user_data, GError **error)
{
  int has_attr, i;
  char *ptr;
  struct Background *tmpbg;
  char *tmpbg_filename;
  GtkWidget *dialog;
  
  if (!strcmp(element_name, "title") || !strcmp(element_name, "xournal")) {
    if (tmpPage != NULL) {
      *error = xoj_invalid();
      return;
    }
    // nothing special to do
  }
  else if (!strcmp(element_name, "page")) { // start of a page
    if (tmpPage != NULL) {
      *error = xoj_invalid();
      return;
    }
    tmpPage = (struct Page *)g_malloc(sizeof(struct Page));
    tmpPage->layers = NULL;
    tmpPage->nlayers = 0;
    tmpPage->group = NULL;
    tmpPage->bg = g_new(struct Background, 1);
    tmpPage->bg->type = -1;
    tmpPage->bg->canvas_item = NULL;
    tmpPage->bg->pixbuf = NULL;
    tmpPage->bg->filename = NULL;
    tmpJournal.pages = g_list_append(tmpJournal.pages, tmpPage);
    tmpJournal.npages++;
    // scan for height and width attributes
    has_attr = 0;
    while (*attribute_names!=NULL) {
      if (!strcmp(*attribute_names, "width")) {
        if (has_attr & 1) *error = xoj_invalid();
        tmpPage->width = strtod(*attribute_values, &ptr);
        if (ptr == *attribute_values) *error = xoj_invalid();
        has_attr |= 1;
      }
      else if (!strcmp(*attribute_names, "height")) {
        if (has_attr & 2) *error = xoj_invalid();
        tmpPage->height = strtod(*attribute_values, &ptr);
        if (ptr == *attribute_values) *error = xoj_invalid();
        has_attr |= 2;
      }
      else *error = xoj_invalid();
      attribute_names++;
      attribute_values++;
    }
    if (has_attr!=3) *error = xoj_invalid();
  }
  else if (!strcmp(element_name, "background")) {
    if (tmpPage == NULL || tmpLayer !=NULL || tmpPage->bg->type >= 0) {
      *error = xoj_invalid();
      return;
    }
    has_attr = 0;
    while (*attribute_names!=NULL) {
      if (!strcmp(*attribute_names, "type")) {
        if (has_attr) *error = xoj_invalid();
        for (i=0; i<3; i++)
          if (!strcmp(*attribute_values, bgtype_names[i]))
            tmpPage->bg->type = i;
        if (tmpPage->bg->type < 0) *error = xoj_invalid();
        has_attr |= 1;
        if (tmpPage->bg->type == BG_PDF) {
          if (tmpBg_pdf == NULL) tmpBg_pdf = tmpPage->bg;
          else {
            has_attr |= 24;
            tmpPage->bg->filename = refstring_ref(tmpBg_pdf->filename);
            tmpPage->bg->file_domain = tmpBg_pdf->file_domain;
          }
        }
      }
      else if (!strcmp(*attribute_names, "color")) {
        if (tmpPage->bg->type != BG_SOLID) *error = xoj_invalid();
        if (has_attr & 2) *error = xoj_invalid();
        tmpPage->bg->color_no = COLOR_OTHER;
        for (i=0; i<COLOR_MAX; i++)
          if (!strcmp(*attribute_values, bgcolor_names[i])) {
            tmpPage->bg->color_no = i;
            tmpPage->bg->color_rgba = predef_bgcolors_rgba[i];
          }
        // there's also the case of hex (#rrggbbaa) colors
        if (tmpPage->bg->color_no == COLOR_OTHER && **attribute_values == '#') {
          tmpPage->bg->color_rgba = strtol(*attribute_values + 1, &ptr, 16);
          if (*ptr!=0) *error = xoj_invalid();
        }
        has_attr |= 2;
      }
      else if (!strcmp(*attribute_names, "style")) {
        if (tmpPage->bg->type != BG_SOLID) *error = xoj_invalid();
        if (has_attr & 4) *error = xoj_invalid();
        tmpPage->bg->ruling = -1;
        for (i=0; i<4; i++)
          if (!strcmp(*attribute_values, bgstyle_names[i]))
            tmpPage->bg->ruling = i;
        if (tmpPage->bg->ruling < 0) *error = xoj_invalid();
        has_attr |= 4;
      }
      else if (!strcmp(*attribute_names, "domain")) {
        if (tmpPage->bg->type <= BG_SOLID || (has_attr & 8))
          { *error = xoj_invalid(); return; }
        tmpPage->bg->file_domain = -1;
        for (i=0; i<3; i++)
          if (!strcmp(*attribute_values, file_domain_names[i]))
            tmpPage->bg->file_domain = i;
        if (tmpPage->bg->file_domain < 0)
          { *error = xoj_invalid(); return; }
        has_attr |= 8;
      }
      else if (!strcmp(*attribute_names, "filename")) {
        if (tmpPage->bg->type <= BG_SOLID || (has_attr != 9)) 
          { *error = xoj_invalid(); return; }
        if (tmpPage->bg->file_domain == DOMAIN_CLONE) {
          // filename is a page number
          i = strtol(*attribute_values, &ptr, 10);
          if (ptr == *attribute_values || i < 0 || i > tmpJournal.npages-2)
            { *error = xoj_invalid(); return; }
          tmpbg = ((struct Page *)g_list_nth_data(tmpJournal.pages, i))->bg;
          if (tmpbg->type != tmpPage->bg->type)
            { *error = xoj_invalid(); return; }
          tmpPage->bg->filename = refstring_ref(tmpbg->filename);
          tmpPage->bg->pixbuf = tmpbg->pixbuf;
          if (tmpbg->pixbuf!=NULL) gdk_pixbuf_ref(tmpbg->pixbuf);
          tmpPage->bg->file_domain = tmpbg->file_domain;
        }
        else {
          tmpPage->bg->filename = new_refstring(*attribute_values);
          if (tmpPage->bg->type == BG_PIXMAP) {
            if (tmpPage->bg->file_domain == DOMAIN_ATTACH) {
              tmpbg_filename = g_strdup_printf("%s.%s", tmpFilename, *attribute_values);
              if (sscanf(*attribute_values, "bg_%d.png", &i) == 1)
                if (i > tmpJournal.last_attach_no) 
                  tmpJournal.last_attach_no = i;
            }
            else tmpbg_filename = g_strdup(*attribute_values);
            tmpPage->bg->pixbuf = gdk_pixbuf_new_from_file(tmpbg_filename, NULL);
            if (tmpPage->bg->pixbuf == NULL) {
              dialog = gtk_message_dialog_new(GTK_WINDOW(winMain), GTK_DIALOG_MODAL,
                GTK_MESSAGE_WARNING, GTK_BUTTONS_OK, 
                "Could not open background '%s'. Setting background to white.",
                tmpbg_filename);
              gtk_dialog_run(GTK_DIALOG(dialog));
              gtk_widget_destroy(dialog);
              tmpPage->bg->pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 1, 1);
              gdk_pixbuf_fill(tmpPage->bg->pixbuf, 0xffffffff); // solid white
            }
            g_free(tmpbg_filename);
          }
        }
        has_attr |= 16;
      }
      else if (!strcmp(*attribute_names, "pageno")) {
        if (tmpPage->bg->type != BG_PDF || (has_attr & 32))
          { *error = xoj_invalid(); return; }
        tmpPage->bg->file_page_seq = strtod(*attribute_values, &ptr);
        if (ptr == *attribute_values) *error = xoj_invalid();
        has_attr |= 32;
      }
      else *error = xoj_invalid();
      attribute_names++;
      attribute_values++;
    }
    if (tmpPage->bg->type < 0) *error = xoj_invalid();
    if (tmpPage->bg->type == BG_SOLID && has_attr != 7) *error = xoj_invalid();
    if (tmpPage->bg->type == BG_PIXMAP && has_attr != 25) *error = xoj_invalid();
    if (tmpPage->bg->type == BG_PDF && has_attr != 57) *error = xoj_invalid();
  }
  else if (!strcmp(element_name, "layer")) { // start of a layer
    if (tmpPage == NULL || tmpLayer != NULL) {
      *error = xoj_invalid();
      return;
    }
    tmpLayer = (struct Layer *)g_malloc(sizeof(struct Layer));
    tmpLayer->items = NULL;
    tmpLayer->nitems = 0;
    tmpLayer->group = NULL;
    tmpPage->layers = g_list_append(tmpPage->layers, tmpLayer);
    tmpPage->nlayers++;
  }
  else if (!strcmp(element_name, "stroke")) { // start of a stroke
    if (tmpLayer == NULL || tmpItem != NULL) {
      *error = xoj_invalid();
      return;
    }
    tmpItem = (struct Item *)g_malloc(sizeof(struct Item));
    tmpItem->type = ITEM_STROKE;
    tmpItem->path = NULL;
    tmpItem->canvas_item = NULL;
    tmpLayer->items = g_list_append(tmpLayer->items, tmpItem);
    tmpLayer->nitems++;
    // scan for tool, color, and width attributes
    has_attr = 0;
    while (*attribute_names!=NULL) {
      if (!strcmp(*attribute_names, "width")) {
        if (has_attr & 1) *error = xoj_invalid();
        tmpItem->brush.thickness = strtod(*attribute_values, &ptr);
        if (ptr == *attribute_values) *error = xoj_invalid();
        has_attr |= 1;
      }
      else if (!strcmp(*attribute_names, "color")) {
        if (has_attr & 2) *error = xoj_invalid();
        tmpItem->brush.color_no = COLOR_OTHER;
        for (i=0; i<COLOR_MAX; i++)
          if (!strcmp(*attribute_values, color_names[i])) {
            tmpItem->brush.color_no = i;
            tmpItem->brush.color_rgba = predef_colors_rgba[i];
          }
        // there's also the case of hex (#rrggbbaa) colors
        if (tmpItem->brush.color_no == COLOR_OTHER && **attribute_values == '#') {
          tmpItem->brush.color_rgba = strtol(*attribute_values + 1, &ptr, 16);
          if (*ptr!=0) *error = xoj_invalid();
        }
        has_attr |= 2;
      }
      else if (!strcmp(*attribute_names, "tool")) {
        if (has_attr & 4) *error = xoj_invalid();
        tmpItem->brush.tool_type = -1;
        for (i=0; i<NUM_STROKE_TOOLS; i++)
          if (!strcmp(*attribute_values, tool_names[i])) {
            tmpItem->brush.tool_type = i;
          }
        if (tmpItem->brush.tool_type == -1) *error = xoj_invalid();
        has_attr |= 4;
      }
      else *error = xoj_invalid();
      attribute_names++;
      attribute_values++;
    }
    if (has_attr!=7) *error = xoj_invalid();
    // finish filling the brush info
    tmpItem->brush.thickness_no = 0;  // who cares ?
    tmpItem->brush.tool_options = 0;  // who cares ?
    if (tmpItem->brush.tool_type == TOOL_HIGHLIGHTER) {
      if (tmpItem->brush.color_no >= 0)
        tmpItem->brush.color_rgba &= HILITER_ALPHA_MASK;
    }
  }
}

void xoj_parser_end_element(GMarkupParseContext *context,
   const gchar *element_name, gpointer user_data, GError **error)
{
  if (!strcmp(element_name, "page")) {
    if (tmpPage == NULL || tmpLayer != NULL) {
      *error = xoj_invalid();
      return;
    }
    if (tmpPage->nlayers == 0 || tmpPage->bg->type < 0) *error = xoj_invalid();
    tmpPage = NULL;
  }
  if (!strcmp(element_name, "layer")) {
    if (tmpLayer == NULL || tmpItem != NULL) {
      *error = xoj_invalid();
      return;
    }
    tmpLayer = NULL;
  }
  if (!strcmp(element_name, "stroke")) {
    if (tmpItem == NULL) {
      *error = xoj_invalid();
      return;
    }
    update_item_bbox(tmpItem);
    tmpItem = NULL;
  }
}

void xoj_parser_text(GMarkupParseContext *context,
   const gchar *text, gsize text_len, gpointer user_data, GError **error)
{
  const gchar *element_name, *ptr;
  int n;
  
  element_name = g_markup_parse_context_get_element(context);
  if (element_name == NULL) return;
  if (!strcmp(element_name, "stroke")) {
    ptr = text;
    n = 0;
    while (text_len > 0) {
      realloc_cur_path(n/2 + 1);
      ui.cur_path.coords[n] = strtod(text, (char **)(&ptr));
      if (ptr == text) break;
      text_len -= (ptr - text);
      text = ptr;
      n++;
    }
    if (n<4 || n&1) { *error = xoj_invalid(); return; }
    tmpItem->path = gnome_canvas_points_new(n/2);
    g_memmove(tmpItem->path->coords, ui.cur_path.coords, n*sizeof(double));
  }
}

gboolean user_wants_second_chance(char **filename)
{
  GtkWidget *dialog;
  GtkFileFilter *filt_all, *filt_pdf;
  GtkResponseType response;

  dialog = gtk_message_dialog_new(GTK_WINDOW(winMain), GTK_DIALOG_MODAL,
    GTK_MESSAGE_ERROR, GTK_BUTTONS_YES_NO, 
    "Could not open background '%s'.\nSelect another file?",
    *filename);
  response = gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_widget_destroy(dialog);
  if (response != GTK_RESPONSE_YES) return FALSE;
  dialog = gtk_file_chooser_dialog_new("Open PDF", GTK_WINDOW (winMain),
     GTK_FILE_CHOOSER_ACTION_OPEN, GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
     GTK_STOCK_OPEN, GTK_RESPONSE_ACCEPT, NULL);

  filt_all = gtk_file_filter_new();
  gtk_file_filter_set_name(filt_all, "All files");
  gtk_file_filter_add_pattern(filt_all, "*");
  filt_pdf = gtk_file_filter_new();
  gtk_file_filter_set_name(filt_pdf, "PDF files");
  gtk_file_filter_add_pattern(filt_pdf, "*.pdf");
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER (dialog), filt_pdf);
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER (dialog), filt_all);

  if (gtk_dialog_run(GTK_DIALOG(dialog)) != GTK_RESPONSE_ACCEPT) {
    gtk_widget_destroy(dialog);
    return FALSE;
  }
  g_free(*filename);
  *filename = gtk_file_chooser_get_filename (GTK_FILE_CHOOSER (dialog));
  gtk_widget_destroy(dialog);
  return TRUE;    
}

gboolean open_journal(char *filename)
{
  const GMarkupParser parser = { xoj_parser_start_element, 
                                 xoj_parser_end_element, 
                                 xoj_parser_text, NULL, NULL};
  GMarkupParseContext *context;
  GError *error;
  GtkWidget *dialog;
  gboolean valid;
  gzFile f;
  char buffer[1000];
  int len;
  gchar *tmpfn;
  gboolean maybe_pdf;
  
  f = gzopen(filename, "r");
  if (f==NULL) return FALSE;
  
  context = g_markup_parse_context_new(&parser, 0, NULL, NULL);
  valid = TRUE;
  tmpJournal.npages = 0;
  tmpJournal.pages = NULL;
  tmpJournal.last_attach_no = 0;
  tmpPage = NULL;
  tmpLayer = NULL;
  tmpItem = NULL;
  tmpFilename = filename;
  error = NULL;
  tmpBg_pdf = NULL;
  maybe_pdf = TRUE;

  while (valid && !gzeof(f)) {
    len = gzread(f, buffer, 1000);
    if (len<0) valid = FALSE;
    if (maybe_pdf && len>=4 && !strncmp(buffer, "%PDF", 4))
      { valid = FALSE; break; } // most likely pdf
    else maybe_pdf = FALSE;
    if (len<=0) break;
    valid = g_markup_parse_context_parse(context, buffer, len, &error);
  }
  gzclose(f);
  if (valid) valid = g_markup_parse_context_end_parse(context, &error);
  if (tmpJournal.npages == 0) valid = FALSE;
  g_markup_parse_context_free(context);
  
  if (!valid) {
    delete_journal(&tmpJournal);
    if (!maybe_pdf) return FALSE;
    // essentially same as on_fileNewBackground from here on
    ui.saved = TRUE;
    close_journal();
    while (bgpdf.status != STATUS_NOT_INIT) gtk_main_iteration();
    new_journal();
    ui.zoom = DEFAULT_ZOOM;
    gnome_canvas_set_pixels_per_unit(canvas, ui.zoom);
    update_page_stuff();
    return init_bgpdf(filename, TRUE, DOMAIN_ABSOLUTE);
  }
  
  ui.saved = TRUE; // force close_journal() to do its job
  close_journal();
  g_memmove(&journal, &tmpJournal, sizeof(struct Journal));
  
  // if we need to initialize a fresh pdf loader
  if (tmpBg_pdf!=NULL) { 
    while (bgpdf.status != STATUS_NOT_INIT) gtk_main_iteration();
    if (tmpBg_pdf->file_domain == DOMAIN_ATTACH)
      tmpfn = g_strdup_printf("%s.%s", filename, tmpBg_pdf->filename->s);
    else
      tmpfn = g_strdup(tmpBg_pdf->filename->s);
    valid = init_bgpdf(tmpfn, FALSE, tmpBg_pdf->file_domain);
    // in case the file name became invalid
    if (!valid && tmpBg_pdf->file_domain != DOMAIN_ATTACH)
      if (user_wants_second_chance(&tmpfn)) {
        valid = init_bgpdf(tmpfn, FALSE, tmpBg_pdf->file_domain);
        if (valid) { // change the file name...
          g_free(tmpBg_pdf->filename->s);
          tmpBg_pdf->filename->s = g_strdup(tmpfn);
        }
      }
    if (valid) {
      refstring_unref(bgpdf.filename);
      bgpdf.filename = refstring_ref(tmpBg_pdf->filename);
    } else {
      dialog = gtk_message_dialog_new(GTK_WINDOW(winMain), GTK_DIALOG_MODAL,
        GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "Could not open background '%s'.",
        tmpfn);
      gtk_dialog_run(GTK_DIALOG(dialog));
      gtk_widget_destroy(dialog);
    }
    g_free(tmpfn);
  }
  
  ui.pageno = 0;
  ui.cur_page = (struct Page *)journal.pages->data;
  ui.layerno = ui.cur_page->nlayers-1;
  ui.cur_layer = (struct Layer *)(g_list_last(ui.cur_page->layers)->data);
  ui.saved = TRUE;
  ui.zoom = DEFAULT_ZOOM;
  update_file_name(g_strdup(filename));
  gnome_canvas_set_pixels_per_unit(canvas, ui.zoom);
  make_canvas_items();
  update_page_stuff();
  gtk_adjustment_set_value(gtk_layout_get_vadjustment(GTK_LAYOUT(canvas)), 0);
  return TRUE;
}

/************ file backgrounds *************/

struct Background *attempt_load_pix_bg(char *filename, gboolean attach)
{
  struct Background *bg;
  GdkPixbuf *pix;
  
  pix = gdk_pixbuf_new_from_file(filename, NULL);
  if (pix == NULL) return NULL;
  
  bg = g_new(struct Background, 1);
  bg->type = BG_PIXMAP;
  bg->canvas_item = NULL;
  bg->pixbuf = pix;
  bg->pixbuf_scale = DEFAULT_ZOOM;
  if (attach) {
    bg->filename = new_refstring(NULL);
    bg->file_domain = DOMAIN_ATTACH;
  } else {
    bg->filename = new_refstring(filename);
    bg->file_domain = DOMAIN_ABSOLUTE;
  }
  return bg;
}

#define BUFSIZE 65536 // a reasonable buffer size for reads from gs pipe

GList *attempt_load_gv_bg(char *filename)
{
  struct Background *bg;
  GList *bg_list;
  GdkPixbuf *pix;
  GdkPixbufLoader *loader;
  FILE *gs_pipe, *f;
  unsigned char *buf;
  char *pipename;
  int buflen, remnlen, file_pageno;
  
  buf = g_malloc(BUFSIZE); // a reasonable buffer size
  f = fopen(filename, "r");
  if (fread(buf, 1, 4, f) !=4 ||
        (strncmp((char *)buf, "%!PS", 4) && strncmp((char *)buf, "%PDF", 4))) {
    fclose(f);
    g_free(buf);
    return NULL;
  }
  
  fclose(f);
  pipename = g_strdup_printf(GS_CMDLINE, (double)GS_BITMAP_DPI, filename);
  gs_pipe = popen(pipename, "r");
  g_free(pipename);
  
  bg_list = NULL;
  remnlen = 0;
  file_pageno = 0;
  loader = NULL;
  if (gs_pipe!=NULL)
  while (!feof(gs_pipe)) {
    if (!remnlen) { // new page: get a BMP header ?
      buflen = fread(buf, 1, 54, gs_pipe);
      if (buflen < 6) buflen += fread(buf, 1, 54-buflen, gs_pipe);
      if (buflen < 6 || buf[0]!='B' || buf[1]!='M') break; // fatal: abort
      remnlen = (int)(buf[5]<<24) + (buf[4]<<16) + (buf[3]<<8) + (buf[2]);
      loader = gdk_pixbuf_loader_new();
    }
    else buflen = fread(buf, 1, (remnlen < BUFSIZE)?remnlen:BUFSIZE, gs_pipe);
    remnlen -= buflen;
    if (buflen == 0) break;
    if (!gdk_pixbuf_loader_write(loader, buf, buflen, NULL)) break;
    if (remnlen == 0) { // make a new bg
      pix = gdk_pixbuf_loader_get_pixbuf(loader);
      if (pix == NULL) break;
      gdk_pixbuf_ref(pix);
      gdk_pixbuf_loader_close(loader, NULL);
      g_object_unref(loader);
      loader = NULL;
      bg = g_new(struct Background, 1);
      bg->canvas_item = NULL;
      bg->pixbuf = pix;
      bg->pixbuf_scale = (GS_BITMAP_DPI/72.0);
      bg->type = BG_PIXMAP;
      bg->filename = new_refstring(NULL);
      bg->file_domain = DOMAIN_ATTACH;
      file_pageno++;
      bg_list = g_list_append(bg_list, bg);
    }
  }
  if (loader != NULL) gdk_pixbuf_loader_close(loader, NULL);
  pclose(gs_pipe);
  g_free(buf);
  return bg_list;
}

struct Background *attempt_screenshot_bg(void)
{
  struct Background *bg;
  GdkPixbuf *pix;
  XEvent x_event;
  GError *error = NULL;
  GdkWindow *window;
  int x,y,w,h, status;
  unsigned int tmp;
  Window x_root, x_win;

  x_root = gdk_x11_get_default_root_xwindow();
  
  if (!XGrabButton(GDK_DISPLAY(), AnyButton, AnyModifier, x_root, 
      False, ButtonReleaseMask, GrabModeAsync, GrabModeSync, None, None))
    return NULL;

  XWindowEvent (GDK_DISPLAY(), x_root, ButtonReleaseMask, &x_event);
  XUngrabButton(GDK_DISPLAY(), AnyButton, AnyModifier, x_root);

  x_win = x_event.xbutton.subwindow;
  if (x_win == None) x_win = x_root;

  window = gdk_window_foreign_new_for_display(gdk_display_get_default(), x_win);
    
  gdk_window_get_geometry(window, &x, &y, &w, &h, NULL);
  
  pix = gdk_pixbuf_get_from_drawable(NULL, window,
    gdk_colormap_get_system(), 0, 0, 0, 0, w, h);
    
  if (pix == NULL) return NULL;
  
  bg = g_new(struct Background, 1);
  bg->type = BG_PIXMAP;
  bg->canvas_item = NULL;
  bg->pixbuf = pix;
  bg->pixbuf_scale = DEFAULT_ZOOM;
  bg->filename = new_refstring(NULL);
  bg->file_domain = DOMAIN_ATTACH;
  return bg;
}

/************** pdf annotation ***************/

/* free tmp directory */

void end_bgpdf_shutdown(void)
{
  if (bgpdf.tmpdir!=NULL) {
    if (bgpdf.tmpfile_copy!=NULL) {
      g_unlink(bgpdf.tmpfile_copy);
      g_free(bgpdf.tmpfile_copy);
      bgpdf.tmpfile_copy = NULL;
    }
    g_rmdir(bgpdf.tmpdir);  
    g_free(bgpdf.tmpdir);
    bgpdf.tmpdir = NULL;
  }
  bgpdf.status = STATUS_NOT_INIT;
}

/* cancel a request */

void cancel_bgpdf_request(struct BgPdfRequest *req)
{
  GList *list_link;
  
  list_link = g_list_find(bgpdf.requests, req);
  if (list_link == NULL) return;
  if (list_link->prev == NULL && bgpdf.pid > 0) {
    // this is being processed: kill the child but don't remove the request yet
    if (bgpdf.status == STATUS_RUNNING) bgpdf.status = STATUS_ABORTED;
    kill(bgpdf.pid, SIGHUP);
//    printf("Cancelling a request - killing %d\n", bgpdf.pid);
  }
  else {
    // remove the request
    bgpdf.requests = g_list_delete_link(bgpdf.requests, list_link);
    g_free(req);
//    printf("Cancelling a request - no kill needed\n");
  }
}

/* sigchld callback */

void bgpdf_child_handler(GPid pid, gint status, gpointer data)
{
  struct BgPdfRequest *req;
  struct BgPdfPage *bgpg;
  gchar *ppm_name;
  GdkPixbuf *pixbuf;
  
  if (bgpdf.requests == NULL) return;
  req = (struct BgPdfRequest *)bgpdf.requests->data;
  
  ppm_name = g_strdup_printf("%s/p-%06d.ppm", bgpdf.tmpdir, req->pageno);
//  printf("Child %d finished, should look for %s... \n", pid, ppm_name);
  
  if (bgpdf.status == STATUS_ABORTED || bgpdf.status == STATUS_SHUTDOWN)
     pixbuf = NULL;
  else
     pixbuf = gdk_pixbuf_new_from_file(ppm_name, NULL);

  unlink(ppm_name);
  g_free(ppm_name);

  if (pixbuf != NULL) { // success
//    printf("success\n");
    while (req->pageno > bgpdf.npages) {
      bgpg = g_new(struct BgPdfPage, 1);
      bgpg->pixbuf = NULL;
      bgpdf.pages = g_list_append(bgpdf.pages, bgpg);
      bgpdf.npages++;
    }
    bgpg = g_list_nth_data(bgpdf.pages, req->pageno-1);
    if (bgpg->pixbuf!=NULL) gdk_pixbuf_unref(bgpg->pixbuf);
    bgpg->pixbuf = pixbuf;
    bgpg->dpi = req->dpi;
    if (req->initial_request && bgpdf.create_pages) {
      bgpdf_create_page_with_bg(req->pageno, bgpg);
      // create page n, resize it, set its bg - all without any undo effect
    } else {
      if (!req->is_printing) bgpdf_update_bg(req->pageno, bgpg);
      // look for all pages with this bg, and update their bg pixmaps
    }
  }
  else {
//    printf("failed or aborted\n");
    bgpdf.create_pages = FALSE;
    req->initial_request = FALSE;
  }

  bgpdf.pid = 0;
  g_spawn_close_pid(pid);
  
  if (req->initial_request)
    req->pageno++; // try for next page
  else
    bgpdf.requests = g_list_delete_link(bgpdf.requests, bgpdf.requests);
  
  if (bgpdf.status == STATUS_SHUTDOWN) {
    end_bgpdf_shutdown();
    return;
  }
  
  bgpdf.status = STATUS_IDLE;
  if (bgpdf.requests != NULL) bgpdf_spawn_child();
}

/* spawn a child to process the head request */

void bgpdf_spawn_child(void)
{
  struct BgPdfRequest *req;
  GPid pid;
  gchar pageno_str[10], dpi_str[10];
  gchar *pdf_filename = bgpdf.tmpfile_copy;
  gchar *ppm_root = g_strdup_printf("%s/p", bgpdf.tmpdir);
  gchar *argv[]= PDFTOPPM_ARGV;
  GtkWidget *dialog;

  if (bgpdf.requests == NULL) return;
  req = (struct BgPdfRequest *)bgpdf.requests->data;
  if (req->pageno > bgpdf.npages+1 || 
      (!req->initial_request && req->pageno <= bgpdf.npages && 
       req->dpi == ((struct BgPdfPage *)g_list_nth_data(bgpdf.pages, req->pageno-1))->dpi))
  { // ignore this request - it's redundant, or in outer space
    bgpdf.pid = 0;
    bgpdf.status = STATUS_IDLE;
    bgpdf.requests = g_list_delete_link(bgpdf.requests, bgpdf.requests);
    g_free(ppm_root);
    if (bgpdf.requests != NULL) bgpdf_spawn_child();
    return;
  }
  g_snprintf(pageno_str, 10, "%d", req->pageno);
  g_snprintf(dpi_str, 10, "%d", req->dpi);
/*  printf("Processing request for page %d at %d dpi -- in %s\n", 
    req->pageno, req->dpi, ppm_root); */
  if (!g_spawn_async(NULL, argv, NULL,
                     G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_SEARCH_PATH, 
                     NULL, NULL, &pid, NULL))
  {
    // couldn't spawn... abort this request, try next one maybe ?
//    printf("Couldn't spawn\n");
    bgpdf.pid = 0;
    bgpdf.status = STATUS_IDLE;
    bgpdf.requests = g_list_delete_link(bgpdf.requests, bgpdf.requests);
    g_free(ppm_root);
    if (!bgpdf.has_failed) {
      dialog = gtk_message_dialog_new(GTK_WINDOW(winMain), GTK_DIALOG_MODAL,
        GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "Unable to start PDF loader %s.", argv[0]);
      gtk_dialog_run(GTK_DIALOG(dialog));
      gtk_widget_destroy(dialog);
    }
    bgpdf.has_failed = TRUE;
    if (bgpdf.requests != NULL) bgpdf_spawn_child();
    return;
  }  

//  printf("Spawned process %d\n", pid);
  bgpdf.pid = pid;
  bgpdf.status = STATUS_RUNNING;
  g_child_watch_add(pid, bgpdf_child_handler, NULL);
  g_free(ppm_root);
}

/* make a request */

void add_bgpdf_request(int pageno, double zoom, gboolean printing)
{
  struct BgPdfRequest *req, *cmp_req;
  GList *list;
  
  if (bgpdf.status == STATUS_NOT_INIT || bgpdf.status == STATUS_SHUTDOWN)
    return; // don't accept requests in those modes...
  req = g_new(struct BgPdfRequest, 1);
  req->is_printing = printing;
  if (printing) req->dpi = PDFTOPPM_PRINTING_DPI;
  else req->dpi = (int)floor(72*zoom+0.5);
//  printf("Enqueuing request for page %d at %d dpi\n", pageno, req->dpi);
  if (pageno >= 1) {
    // cancel any request this may supersede
    for (list = bgpdf.requests; list != NULL; ) {
      cmp_req = (struct BgPdfRequest *)list->data;
      list = list->next;
      if (!cmp_req->initial_request && cmp_req->pageno == pageno &&
             cmp_req->is_printing == printing)
        cancel_bgpdf_request(cmp_req);
    }
    req->pageno = pageno;
    req->initial_request = FALSE;
  } else {
    req->pageno = 1;
    req->initial_request = TRUE;
  }
  bgpdf.requests = g_list_append(bgpdf.requests, req);
  if (!bgpdf.pid) bgpdf_spawn_child();
}

/* shutdown the PDF reader */

void shutdown_bgpdf(void)
{
  GList *list;
  struct BgPdfPage *pdfpg;
  struct BgPdfRequest *req;
  
  if (bgpdf.status == STATUS_NOT_INIT || bgpdf.status == STATUS_SHUTDOWN) return;
  refstring_unref(bgpdf.filename);
  for (list = bgpdf.pages; list != NULL; list = list->next) {
    pdfpg = (struct BgPdfPage *)list->data;
    if (pdfpg->pixbuf!=NULL) gdk_pixbuf_unref(pdfpg->pixbuf);
  }
  g_list_free(bgpdf.pages);
  bgpdf.status = STATUS_SHUTDOWN;
  for (list = g_list_last(bgpdf.requests); list != NULL; ) {
    req = (struct BgPdfRequest *)list->data;
    list = list->prev;
    cancel_bgpdf_request(req);
  }
  if (!bgpdf.pid) end_bgpdf_shutdown();
  /* The above will ultimately remove all requests and kill the child if needed.
     The child will set status to STATUS_NOT_INIT, clear the requests list,
     empty tmpdir, ... except if there's no child! */
  /* note: it could look like there's a race condition here - if a child
     terminates and a new request is enqueued while we are destroying the
     queue - but actually the child handler callback is NOT a signal
     callback, so execution of this function is atomic */
}

gboolean init_bgpdf(char *pdfname, gboolean create_pages, int file_domain)
{
  FILE *f;
  gchar *filebuf;
  gsize filelen;
  
  if (bgpdf.status != STATUS_NOT_INIT) return FALSE;
  bgpdf.tmpfile_copy = NULL;
  bgpdf.tmpdir = mkdtemp(g_strdup(TMPDIR_TEMPLATE));
  if (!bgpdf.tmpdir) return FALSE;
  // make a local copy and check if it's a PDF
  if (!g_file_get_contents(pdfname, &filebuf, &filelen, NULL))
    { end_bgpdf_shutdown(); return FALSE; }
  if (filelen < 4 || strncmp(filebuf, "%PDF", 4))
    { g_free(filebuf); end_bgpdf_shutdown(); return FALSE; }
  bgpdf.tmpfile_copy = g_strdup_printf("%s/bg.pdf", bgpdf.tmpdir);
  f = fopen(bgpdf.tmpfile_copy, "w");
  if (f == NULL || fwrite(filebuf, 1, filelen, f) != filelen) 
    { g_free(filebuf); end_bgpdf_shutdown(); return FALSE; }
  fclose(f);
  g_free(filebuf);
  bgpdf.status = STATUS_IDLE;
  bgpdf.pid = 0;
  bgpdf.filename = new_refstring((file_domain == DOMAIN_ATTACH) ? "bg.pdf" : pdfname);
  bgpdf.file_domain = file_domain;
  bgpdf.npages = 0;
  bgpdf.pages = NULL;
  bgpdf.requests = NULL;
  bgpdf.create_pages = create_pages;
  bgpdf.has_failed = FALSE;
  add_bgpdf_request(-1, DEFAULT_ZOOM, FALSE); // request all pages
  return TRUE;
}

// create page n, resize it, set its bg
void bgpdf_create_page_with_bg(int pageno, struct BgPdfPage *bgpg)
{
  struct Page *pg;
  struct Background *bg;

  if (journal.npages < pageno) {
    bg = g_new(struct Background, 1);
    bg->canvas_item = NULL;
  } else {
    pg = (struct Page *)g_list_nth_data(journal.pages, pageno-1);
    bg = pg->bg;
    if (bg->type != BG_SOLID) return;
      // don't mess with a page the user has modified significantly...
  }
  
  bg->type = BG_PDF;
  bg->pixbuf = gdk_pixbuf_ref(bgpg->pixbuf);
  bg->filename = refstring_ref(bgpdf.filename);
  bg->file_domain = bgpdf.file_domain;
  bg->file_page_seq = pageno;
  bg->pixbuf_scale = DEFAULT_ZOOM;
  bg->pixbuf_dpi = bgpg->dpi;

  if (journal.npages < pageno) {
    pg = new_page_with_bg(bg, 
            gdk_pixbuf_get_width(bg->pixbuf)*72.0/bg->pixbuf_dpi,
            gdk_pixbuf_get_height(bg->pixbuf)*72.0/bg->pixbuf_dpi);
    journal.pages = g_list_append(journal.pages, pg);
    journal.npages++;
  } else {
    pg->width = gdk_pixbuf_get_width(bgpg->pixbuf)*72.0/bg->pixbuf_dpi;
    pg->height = gdk_pixbuf_get_height(bgpg->pixbuf)*72.0/bg->pixbuf_dpi;
    make_page_clipbox(pg);
    update_canvas_bg(pg);
  }
  update_page_stuff();
}

// look for all journal pages with given pdf bg, and update their bg pixmaps
void bgpdf_update_bg(int pageno, struct BgPdfPage *bgpg)
{
  GList *list;
  struct Page *pg;
  
  for (list = journal.pages; list!= NULL; list = list->next) {
    pg = (struct Page *)list->data;
    if (pg->bg->type == BG_PDF && pg->bg->file_page_seq == pageno) {
      if (pg->bg->pixbuf!=NULL) gdk_pixbuf_unref(pg->bg->pixbuf);
      pg->bg->pixbuf = gdk_pixbuf_ref(bgpg->pixbuf);
      pg->bg->pixbuf_dpi = bgpg->dpi;
      update_canvas_bg(pg);
    }
  }
}

// initialize the recent files list
void init_mru(void)
{
  int i;
  gsize lfptr;
  char s[5];
  GIOChannel *f;
  gchar *str;
  GIOStatus status;
  
  g_strlcpy(s, "mru0", 5);
  for (s[3]='0', i=0; i<MRU_SIZE; s[3]++, i++) {
    ui.mrumenu[i] = GET_COMPONENT(s);
    ui.mru[i] = NULL;
  }
  f = g_io_channel_new_file(ui.mrufile, "r", NULL);
  if (f) status = G_IO_STATUS_NORMAL;
  else status = G_IO_STATUS_ERROR;
  i = 0;
  while (status == G_IO_STATUS_NORMAL && i<MRU_SIZE) {
    lfptr = 0;
    status = g_io_channel_read_line(f, &str, NULL, &lfptr, NULL);
    if (status == G_IO_STATUS_NORMAL && lfptr>0) {
      str[lfptr] = 0;
      ui.mru[i] = str;
      i++;
    }
  }
  if (f) {
    g_io_channel_shutdown(f, FALSE, NULL);
    g_io_channel_unref(f);
  }
  update_mru_menu();
}

void update_mru_menu(void)
{
  int i;
  gboolean anyone = FALSE;
  
  for (i=0; i<MRU_SIZE; i++) {
    if (ui.mru[i]!=NULL) {
      gtk_label_set_text(GTK_LABEL(gtk_bin_get_child(GTK_BIN(ui.mrumenu[i]))),
          g_basename(ui.mru[i]));
      gtk_widget_show(ui.mrumenu[i]);
      anyone = TRUE;
    }
    else gtk_widget_hide(ui.mrumenu[i]);
  }
  gtk_widget_set_sensitive(GET_COMPONENT("fileRecentFiles"), anyone);
}

void new_mru_entry(char *name)
{
  int i, j;
  
  for (i=0;i<MRU_SIZE;i++) 
    if (ui.mru[i]!=NULL && !strcmp(ui.mru[i], name)) {
      g_free(ui.mru[i]);
      for (j=i+1; j<MRU_SIZE; j++) ui.mru[j-1] = ui.mru[j];
      ui.mru[MRU_SIZE-1]=NULL;
    }
  if (ui.mru[MRU_SIZE-1]!=NULL) g_free(ui.mru[MRU_SIZE-1]);
  for (j=MRU_SIZE-1; j>=1; j--) ui.mru[j] = ui.mru[j-1];
  ui.mru[0] = g_strdup(name);
  update_mru_menu();
}

void delete_mru_entry(int which)
{
  int i;
  
  if (ui.mru[which]!=NULL) g_free(ui.mru[which]);
  for (i=which+1;i<MRU_SIZE;i++) 
    ui.mru[i-1] = ui.mru[i];
  ui.mru[MRU_SIZE-1] = NULL;
  update_mru_menu();
}

void save_mru_list(void)
{
  FILE *f;
  int i;
  
  f = fopen(ui.mrufile, "w");
  if (f==NULL) return;
  for (i=0; i<MRU_SIZE; i++)
    if (ui.mru[i]!=NULL) fprintf(f, "%s\n", ui.mru[i]);
  fclose(f);
}