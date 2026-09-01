/*
 * Phi PDF Viewer - Document properties dialog
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "pdfv-document-properties.h"

#include <math.h>
#include <string.h>

typedef struct {
  const gchar *name;
  gdouble width_mm;
  gdouble height_mm;
} PaperSize;

static const PaperSize paper_sizes[] = {
    {"A0", 841.0, 1189.0},
    {"A1", 594.0, 841.0},
    {"A2", 420.0, 594.0},
    {"A3", 297.0, 420.0},
    {"A4", 210.0, 297.0},
    {"A5", 148.0, 210.0},
    {"A6", 105.0, 148.0},
    {"Letter", 215.9, 279.4},
    {"Legal", 215.9, 355.6},
    {"Tabloid", 279.4, 431.8},
};

static const gchar *paper_size_name(gdouble width_mm, gdouble height_mm) {
  gdouble short_side = MIN(width_mm, height_mm);
  gdouble long_side = MAX(width_mm, height_mm);
  for (guint i = 0; i < G_N_ELEMENTS(paper_sizes); i++) {
    if (fabs(short_side - paper_sizes[i].width_mm) <= 2.0 &&
        fabs(long_side - paper_sizes[i].height_mm) <= 2.0)
      return paper_sizes[i].name;
  }
  return NULL;
}

gchar *pdfv_document_properties_format_page_size(gfloat width_points,
                                                 gfloat height_points) {
  if (width_points <= 0.0f || height_points <= 0.0f)
    return g_strdup("Unknown");

  gdouble width_mm = width_points * 25.4 / 72.0;
  gdouble height_mm = height_points * 25.4 / 72.0;
  const gchar *name = paper_size_name(width_mm, height_mm);
  const gchar *orientation = fabs(width_points - height_points) < 0.5
                                 ? "square"
                                 : width_points < height_points
                                       ? "portrait"
                                       : "landscape";
  if (name)
    return g_strdup_printf(
        "%s, %s %.1f × %.1f mm (%.1f × %.1f pt)", name, orientation,
        width_mm, height_mm, width_points, height_points);
  return g_strdup_printf("%.1f × %.1f mm (%.1f × %.1f pt), %s",
                         width_mm, height_mm, width_points, height_points,
                         orientation);
}

static gboolean parse_digits(const gchar **cursor, guint count, gint *value) {
  gint parsed = 0;
  for (guint i = 0; i < count; i++) {
    if (!g_ascii_isdigit((*cursor)[i]))
      return FALSE;
    parsed = parsed * 10 + ((*cursor)[i] - '0');
  }
  *cursor += count;
  *value = parsed;
  return TRUE;
}

gchar *pdfv_document_properties_format_pdf_date(const gchar *value) {
  if (!value || !*value)
    return NULL;

  const gchar *cursor = g_str_has_prefix(value, "D:") ? value + 2 : value;
  gint year = 0;
  gint month = 1;
  gint day = 1;
  gint hour = 0;
  gint minute = 0;
  gint second = 0;
  if (strlen(cursor) < 8 || !parse_digits(&cursor, 4, &year) ||
      !parse_digits(&cursor, 2, &month) ||
      !parse_digits(&cursor, 2, &day))
    return g_strdup(value);

  gint *time_parts[] = {&hour, &minute, &second};
  for (guint i = 0; i < G_N_ELEMENTS(time_parts); i++) {
    if (!g_ascii_isdigit(cursor[0]) || !g_ascii_isdigit(cursor[1]))
      break;
    if (!parse_digits(&cursor, 2, time_parts[i]))
      return g_strdup(value);
  }

  if (!g_date_valid_dmy(day, month, year) || hour > 23 || minute > 59 ||
      second > 59)
    return g_strdup(value);

  gboolean timezone_known = FALSE;
  gint offset_seconds = 0;
  if (*cursor == 'Z' || *cursor == 'z') {
    timezone_known = TRUE;
  } else if (*cursor == '+' || *cursor == '-') {
    gint direction = *cursor++ == '+' ? 1 : -1;
    gint offset_hour = 0;
    gint offset_minute = 0;
    if (parse_digits(&cursor, 2, &offset_hour)) {
      if (*cursor == '\'')
        cursor++;
      if (!parse_digits(&cursor, 2, &offset_minute))
        offset_minute = 0;
      if (offset_hour > 23 || offset_minute > 59)
        return g_strdup(value);
      timezone_known = TRUE;
      offset_seconds = direction * (offset_hour * 3600 + offset_minute * 60);
    }
  }

  GTimeZone *timezone = timezone_known
                            ? g_time_zone_new_offset(offset_seconds)
                            : g_time_zone_new_local();
  GDateTime *date = g_date_time_new(timezone, year, month, day, hour, minute,
                                    second);
  g_time_zone_unref(timezone);
  if (!date)
    return g_strdup(value);
  GDateTime *local = g_date_time_to_local(date);
  gchar *formatted = g_date_time_format(local, "%x, %X");
  g_date_time_unref(local);
  g_date_time_unref(date);
  return formatted;
}

static gchar *format_file_date(GDateTime *date) {
  if (!date)
    return NULL;
  GDateTime *local = g_date_time_to_local(date);
  gchar *formatted = g_date_time_format(local, "%x, %X");
  g_date_time_unref(local);
  return formatted;
}

static void add_property_row(AdwPreferencesGroup *group,
                             const gchar *title,
                             const gchar *value) {
  if (!value || !*value)
    return;
  AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
  adw_action_row_set_subtitle(row, value);
  adw_action_row_set_subtitle_lines(row, 0);
  adw_action_row_set_subtitle_selectable(row, TRUE);
  gtk_widget_set_tooltip_text(GTK_WIDGET(row), value);
  adw_preferences_group_add(group, GTK_WIDGET(row));
}

static gchar *document_page_size(PhiDocument *document, gint page_number) {
  gint pages = phi_document_get_n_pages(document);
  if (pages <= 0)
    return g_strdup("Unknown");
  page_number = CLAMP(page_number, 0, pages - 1);
  PhiPage *page = phi_document_get_page(document, page_number, NULL);
  if (!page)
    return g_strdup("Unknown");
  gfloat width = 0.0f;
  gfloat height = 0.0f;
  phi_page_get_size(page, &width, &height);
  return pdfv_document_properties_format_page_size(width, height);
}

void pdfv_document_properties_present(GtkWidget *parent, GFile *file,
                                      PhiDocument *document,
                                      gint page_number) {
  g_return_if_fail(GTK_IS_WIDGET(parent));
  g_return_if_fail(G_IS_FILE(file));
  g_return_if_fail(PHI_IS_DOCUMENT(document));

  AdwDialog *dialog = adw_preferences_dialog_new();
  adw_dialog_set_title(dialog, "Document Properties");
  adw_dialog_set_content_width(dialog, 560);
  adw_dialog_set_content_height(dialog, 650);

  AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(
      adw_preferences_page_new());
  adw_preferences_page_set_title(page, "Properties");
  adw_preferences_page_set_icon_name(page,
                                     "document-properties-symbolic");
  adw_preferences_dialog_add(ADW_PREFERENCES_DIALOG(dialog), page);

  AdwPreferencesGroup *file_group = ADW_PREFERENCES_GROUP(
      adw_preferences_group_new());
  adw_preferences_group_set_title(file_group, "File");
  adw_preferences_page_add(page, file_group);

  gchar *basename = g_file_get_basename(file);
  gchar *path = g_file_get_path(file);
  if (!path)
    path = g_file_get_parse_name(file);
  add_property_row(file_group, "Name", basename);
  add_property_row(file_group, "Full path", path);

  GError *file_error = NULL;
  GFileInfo *file_info = g_file_query_info(
      file,
      G_FILE_ATTRIBUTE_STANDARD_SIZE ","
      G_FILE_ATTRIBUTE_TIME_CREATED ","
      G_FILE_ATTRIBUTE_TIME_CREATED_USEC ","
      G_FILE_ATTRIBUTE_TIME_MODIFIED ","
      G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC,
      G_FILE_QUERY_INFO_NONE, NULL, &file_error);
  if (file_info) {
    if (g_file_info_has_attribute(file_info,
                                  G_FILE_ATTRIBUTE_STANDARD_SIZE)) {
      gchar *size = g_format_size_full(g_file_info_get_size(file_info),
                                       G_FORMAT_SIZE_IEC_UNITS);
      add_property_row(file_group, "File size", size);
      g_free(size);
    }

    GDateTime *created = g_file_info_has_attribute(
                             file_info, G_FILE_ATTRIBUTE_TIME_CREATED)
                             ? g_file_info_get_creation_date_time(file_info)
                             : NULL;
    GDateTime *modified = g_file_info_has_attribute(
                              file_info, G_FILE_ATTRIBUTE_TIME_MODIFIED)
                              ? g_file_info_get_modification_date_time(
                                    file_info)
                              : NULL;
    gchar *created_text = format_file_date(created);
    gchar *modified_text = format_file_date(modified);
    add_property_row(file_group, "Created", created_text);
    add_property_row(file_group, "Modified", modified_text);
    g_free(created_text);
    g_free(modified_text);
    g_clear_pointer(&created, g_date_time_unref);
    g_clear_pointer(&modified, g_date_time_unref);
    g_object_unref(file_info);
  } else if (file_error) {
    adw_preferences_group_set_description(file_group, file_error->message);
  }
  g_clear_error(&file_error);
  g_free(path);
  g_free(basename);

  AdwPreferencesGroup *document_group = ADW_PREFERENCES_GROUP(
      adw_preferences_group_new());
  adw_preferences_group_set_title(document_group, "Document");
  adw_preferences_page_add(page, document_group);

  gchar *format = phi_document_dup_metadata(
      document, PHI_DOCUMENT_METADATA_FORMAT);
  gchar *encryption = phi_document_dup_metadata(
      document, PHI_DOCUMENT_METADATA_ENCRYPTION);
  gchar *page_count = g_strdup_printf("%d", phi_document_get_n_pages(document));
  gchar *page_size = document_page_size(document, page_number);
  add_property_row(document_group, "Format", format);
  add_property_row(document_group, "Security", encryption);
  add_property_row(document_group, "Page count", page_count);
  add_property_row(document_group, "Current page size", page_size);
  g_free(format);
  g_free(encryption);
  g_free(page_count);
  g_free(page_size);

  AdwPreferencesGroup *metadata_group = ADW_PREFERENCES_GROUP(
      adw_preferences_group_new());
  adw_preferences_group_set_title(metadata_group, "Metadata");
  adw_preferences_page_add(page, metadata_group);

  static const struct {
    PhiDocumentMetadata metadata;
    const gchar *title;
    gboolean date;
  } metadata_rows[] = {
      {PHI_DOCUMENT_METADATA_TITLE, "Title", FALSE},
      {PHI_DOCUMENT_METADATA_AUTHOR, "Author", FALSE},
      {PHI_DOCUMENT_METADATA_SUBJECT, "Subject", FALSE},
      {PHI_DOCUMENT_METADATA_KEYWORDS, "Keywords", FALSE},
      {PHI_DOCUMENT_METADATA_CREATOR, "Creator application", FALSE},
      {PHI_DOCUMENT_METADATA_PRODUCER, "PDF producer", FALSE},
      {PHI_DOCUMENT_METADATA_CREATION_DATE, "Created", TRUE},
      {PHI_DOCUMENT_METADATA_MODIFICATION_DATE, "Modified", TRUE},
  };
  guint metadata_count = 0;
  for (guint i = 0; i < G_N_ELEMENTS(metadata_rows); i++) {
    gchar *raw = phi_document_dup_metadata(document,
                                           metadata_rows[i].metadata);
    gchar *display = metadata_rows[i].date
                         ? pdfv_document_properties_format_pdf_date(raw)
                         : g_strdup(raw);
    if (display && *display) {
      add_property_row(metadata_group, metadata_rows[i].title, display);
      metadata_count++;
    }
    g_free(display);
    g_free(raw);
  }
  if (metadata_count == 0)
    adw_preferences_group_set_description(
        metadata_group, "This PDF does not contain embedded metadata.");

  adw_dialog_present(dialog, parent);
}
