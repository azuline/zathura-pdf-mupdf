/* SPDX-License-Identifier: Zlib */

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <glib-2.0/glib.h>

#include "plugin.h"

#define LENGTH(x) (sizeof(x)/sizeof((x)[0]))

zathura_error_t
pdf_document_open(zathura_document_t* document)
{
  zathura_error_t error = ZATHURA_ERROR_OK;
  if (document == NULL) {
    error = ZATHURA_ERROR_INVALID_ARGUMENTS;
    goto error_ret;
  }

  mupdf_document_t* mupdf_document = calloc(1, sizeof(mupdf_document_t));
  if (mupdf_document == NULL) {
    error = ZATHURA_ERROR_OUT_OF_MEMORY;
    goto error_ret;
  }

  mupdf_document->ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
  if (mupdf_document->ctx == NULL) {
    error = ZATHURA_ERROR_UNKNOWN;
    goto error_free;
  }

  // Custom styles. See:
  // - https://pymupdf.readthedocs.io/en/latest/recipes-text.html#how-to-specify-your-own-fonts
  // - https://github.com/ccxvii/mupdf/blob/master/source/html/html-parse.c#L35
  const char *css = 
      "@font-face { font-family: \"EB Garamond\"; src: url(/home/blissful/fonts/collection/Google/EBGaramond-Regular.ttf); }\n"
      "@font-face { font-family: \"EB Garamond\"; font-weight: bold; src: url(/home/blissful/fonts/collection/Google/EBGaramond-Bold.ttf); }\n"
      "@font-face { font-family: \"EB Garamond\"; font-style: italic; src: url(/home/blissful/fonts/collection/Google/EBGaramond-Italic.ttf); }\n"
      "@font-face { font-family: \"EB Garamond\"; font-weight: bold; font-style: italic; src: url(/home/blissful/fonts/collection/Google/EBGaramond-BoldItalic.ttf); }\n"
      "@page { font-family: \"EB Garamond\"; line-height: 1.4; margin: 2em 6em; }\n"
      "p { margin: 0.5em 0; }\n"
      ;
  fz_set_user_css(mupdf_document->ctx, css);

  /* open document */
  const char* path     = zathura_document_get_path(document);
  const char* password = zathura_document_get_password(document);

  fz_try(mupdf_document->ctx){
    fz_register_document_handlers(mupdf_document->ctx);

    mupdf_document->document = fz_open_document(mupdf_document->ctx, path);
  }
  fz_catch(mupdf_document->ctx){
    error = ZATHURA_ERROR_UNKNOWN;
    return error;
  }

  if (mupdf_document->document == NULL) {
    error = ZATHURA_ERROR_UNKNOWN;
    goto error_free;
  }

  /* authenticate if password is required and given */
  if (fz_needs_password(mupdf_document->ctx, mupdf_document->document) != 0) {
    if (password == NULL || fz_authenticate_password(mupdf_document->ctx, mupdf_document->document, password) == 0) {
      error = ZATHURA_ERROR_INVALID_PASSWORD;
      goto error_free;
    }
  }

  zathura_document_set_number_of_pages(document, fz_count_pages(mupdf_document->ctx, mupdf_document->document));
  zathura_document_set_data(document, mupdf_document);

  return ZATHURA_ERROR_OK;

error_free:

  if (mupdf_document != NULL) {
    if (mupdf_document->document != NULL) {
      fz_drop_document(mupdf_document->ctx, mupdf_document->document);
    }
    if (mupdf_document->ctx != NULL) {
      fz_drop_context(mupdf_document->ctx);
    }

    free(mupdf_document);
  }

  zathura_document_set_data(document, NULL);

error_ret:

  return error;
}

zathura_error_t
pdf_document_free(zathura_document_t* document, void* data)
{
  mupdf_document_t* mupdf_document = data;

  if (document == NULL || mupdf_document == NULL) {
    return ZATHURA_ERROR_INVALID_ARGUMENTS;
  }

  fz_drop_document(mupdf_document->ctx, mupdf_document->document);
  fz_drop_context(mupdf_document->ctx);
  free(mupdf_document);
  zathura_document_set_data(document, NULL);

  return ZATHURA_ERROR_OK;
}

zathura_error_t
pdf_document_save_as(zathura_document_t* document, void* data, const char* path)
{
  mupdf_document_t* mupdf_document = data;

  if (document == NULL || mupdf_document == NULL || path == NULL) {
    return ZATHURA_ERROR_INVALID_ARGUMENTS;
  }

  fz_try (mupdf_document->ctx) {
    pdf_save_document(mupdf_document->ctx, (pdf_document*) mupdf_document->document, path, NULL);
  } fz_catch (mupdf_document->ctx) {
    return ZATHURA_ERROR_UNKNOWN;
  }

  return ZATHURA_ERROR_OK;
}

girara_list_t*
pdf_document_get_information(zathura_document_t* document, void* data, zathura_error_t* error)
{
  mupdf_document_t* mupdf_document = data;

  if (document == NULL || mupdf_document == NULL || error == NULL) {
    if (error != NULL) {
      *error = ZATHURA_ERROR_INVALID_ARGUMENTS;
    }
  }

  girara_list_t* list = zathura_document_information_entry_list_new();
  if (list == NULL) {
    if (error != NULL) {
      *error = ZATHURA_ERROR_UNKNOWN;
    }
    return NULL;
  }

  fz_try (mupdf_document->ctx) {
    pdf_document* pdf_document = pdf_specifics(mupdf_document->ctx, mupdf_document->document);
    if (pdf_document == NULL) {
      girara_list_free(list);
      list = NULL;
      break;
    }

    pdf_obj* trailer = pdf_trailer(mupdf_document->ctx, pdf_document);
    pdf_obj* info_dict = pdf_dict_get(mupdf_document->ctx, trailer, PDF_NAME(Info));

    /* get string values */
    typedef struct info_value_s {
      const char* property;
      zathura_document_information_type_t type;
    } info_value_t;

    static const info_value_t string_values[] = {
      { "Title",    ZATHURA_DOCUMENT_INFORMATION_TITLE },
      { "Author",   ZATHURA_DOCUMENT_INFORMATION_AUTHOR },
      { "Subject",  ZATHURA_DOCUMENT_INFORMATION_SUBJECT },
      { "Keywords", ZATHURA_DOCUMENT_INFORMATION_KEYWORDS },
      { "Creator",  ZATHURA_DOCUMENT_INFORMATION_CREATOR },
      { "Producer", ZATHURA_DOCUMENT_INFORMATION_PRODUCER }
    };

    for (unsigned int i = 0; i < LENGTH(string_values); i++) {
      pdf_obj* value = pdf_dict_gets(mupdf_document->ctx, info_dict, string_values[i].property);
      if (value == NULL) {
        continue;
      }

      char* str_value = pdf_to_str_buf(mupdf_document->ctx, value);
      if (str_value == NULL || strlen(str_value) == 0) {
        continue;
      }

      zathura_document_information_entry_t* entry =
        zathura_document_information_entry_new(
          string_values[i].type,
          str_value
        );

    if (entry != NULL) {
      girara_list_append(list, entry);
    }
    }

    static const info_value_t time_values[] = {
      { "CreationDate", ZATHURA_DOCUMENT_INFORMATION_CREATION_DATE },
      { "ModDate",      ZATHURA_DOCUMENT_INFORMATION_MODIFICATION_DATE }
    };

    for (unsigned int i = 0; i < LENGTH(time_values); i++) {
      pdf_obj* value = pdf_dict_gets(mupdf_document->ctx, info_dict, time_values[i].property);
      if (value == NULL) {
        continue;
      }

      char* str_value = pdf_to_str_buf(mupdf_document->ctx, value);
      if (str_value == NULL || strlen(str_value) == 0) {
        continue;
      }

      zathura_document_information_entry_t* entry =
        zathura_document_information_entry_new(
          time_values[i].type,
          str_value // FIXME: Convert to common format
        );

      if (entry != NULL) {
        girara_list_append(list, entry);
      }
    }
  } fz_catch (mupdf_document->ctx) {
    if (error != NULL) {
      *error = ZATHURA_ERROR_UNKNOWN;
    }
    return NULL;
  }

  return list;
}
