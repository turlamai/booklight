// Printing via GtkPrintOperation — see printing.hpp.
#include "printing.hpp"

#include <algorithm>
#include <cstdio>

namespace pdfgist::printing {
namespace {

struct PrintJob {
  std::shared_ptr<PdfDocument> doc;
};

// Renders `page_nr` through the document's cairo/PDFium path at the print
// context resolution, scaled down to fit the printable area (mac
// .pageScaleDownToFit: never magnifies).
void on_draw_page(GtkPrintOperation*, GtkPrintContext* context, int page_nr,
                  gpointer user_data) {
  auto* job = static_cast<PrintJob*>(user_data);
  cairo_t* cr = gtk_print_context_get_cairo_context(context);
  double const area_w = gtk_print_context_get_width(context);
  double const area_h = gtk_print_context_get_height(context);
  double pw = 0, ph = 0;
  if (job->doc == nullptr || !job->doc->page_size(page_nr, &pw, &ph)) return;
  if (pw <= 0 || ph <= 0 || area_w <= 0 || area_h <= 0) return;
  double const fit = std::min({1.0, area_w / pw, area_h / ph});

  // Render at the printer's device resolution: px-per-point = fit * dpi/72.
  // The context's cairo (GTK_UNITS_POINTS) maps 1 unit to 1 point, so an
  // extra 1/scale draw transform puts surface pixels on device pixels 1:1.
  double const scale =
      std::max(fit * gtk_print_context_get_dpi_x(context) / 72.0, 0.1);
  int surface_w = 0, surface_h = 0;
  cairo_surface_t* surface =
      job->doc->render(page_nr, scale, /*dark=*/false, &surface_w, &surface_h);
  if (surface == nullptr) return;
  cairo_save(cr);
  cairo_translate(cr, (area_w - pw * fit) / 2.0, (area_h - ph * fit) / 2.0);
  cairo_scale(cr, 1.0 / scale, 1.0 / scale);
  cairo_set_source_surface(cr, surface, 0, 0);
  cairo_paint(cr);
  cairo_restore(cr);
  cairo_surface_destroy(surface);
}

}  // namespace

std::string print_document(GtkWindow* parent,
                           std::shared_ptr<PdfDocument> const& doc,
                           int current_page) {
  if (doc == nullptr || doc->page_count() <= 0) {
    return "no document";
  }
  PrintJob job{doc};
  GtkPrintOperation* operation = gtk_print_operation_new();
  gtk_print_operation_set_n_pages(operation, doc->page_count());
  gtk_print_operation_set_current_page(operation,
                                       std::clamp(current_page, 1,
                                                  doc->page_count()) - 1);
  gtk_print_operation_set_unit(operation, GTK_UNIT_POINTS);
  gtk_print_operation_set_job_name(operation, "Booklight");
  gtk_print_operation_set_embed_page_setup(operation, TRUE);
  g_signal_connect_data(operation, "draw-page",
                        reinterpret_cast<GCallback>(+[](GtkPrintOperation* op,
                                                        GtkPrintContext* ctx,
                                                        int page_nr,
                                                        gpointer data) {
                          on_draw_page(op, ctx, page_nr, data);
                        }),
                        &job, nullptr, static_cast<GConnectFlags>(0));
  GError* error = nullptr;
  gtk_print_operation_run(operation, GTK_PRINT_OPERATION_ACTION_PRINT_DIALOG,
                          parent, &error);
  std::string failure;
  if (error != nullptr) {
    failure = error->message != nullptr ? error->message : "print failed";
    g_error_free(error);
  }
  g_object_unref(operation);
  return failure;
}

}  // namespace pdfgist::printing
