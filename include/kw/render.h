/* Renderers: one report, three formats (text for people, JSON for tools and
 * CI, DOT for Graphviz). */
#ifndef KW_RENDER_H
#define KW_RENDER_H

#include "kw/analyze.h"
#include "kw/model.h"
#include "kw/util.h"

typedef struct {
    const kw_snapshot *snap;
    const kw_graph *graph;
    const kw_findings *findings;
} kw_report;

void kw_render_text(const kw_report *r, kw_strbuf *out);
void kw_render_json(const kw_report *r, kw_strbuf *out);
void kw_render_dot(const kw_report *r, kw_strbuf *out);

#endif
