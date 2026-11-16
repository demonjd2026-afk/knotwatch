#include "kw/render.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

static size_t count_observed(const kw_graph *g)
{
    size_t n = 0;
    for (size_t i = 0; i < g->n_edges; i++) {
        n += g->edges[i].observed ? 1 : 0;
    }
    return n;
}

void kw_render_text(const kw_report *r, kw_strbuf *out)
{
    char dur[32];
    kw_format_duration(r->snap->scan_ns, dur, sizeof(dur));
    kw_sb_puts(out, "knotwatch report\n");
    kw_sb_printf(out, "  scan: %zu processes, %zu threads, %zu connections in %s\n",
                 r->snap->n_procs, r->snap->n_threads, r->snap->n_conns, dur);
    kw_sb_printf(out, "  send graph: %zu processes, %zu edges (%zu seen in use)\n\n",
                 r->graph->n_nodes, r->graph->n_edges, count_observed(r->graph));

    if (r->findings->n == 0) {
        kw_sb_puts(out, "No findings. No deadlock, and the send hierarchy has no "
                        "cycles.\n");
        return;
    }
    for (size_t i = 0; i < r->findings->n; i++) {
        const kw_finding *f = &r->findings->items[i];
        char sev[16];
        snprintf(sev, sizeof(sev), "%s", kw_severity_name(kw_finding_severity(f->kind)));
        for (char *p = sev; *p; p++) {
            *p = (char)toupper((unsigned char)*p);
        }
        kw_sb_printf(out, "[%s] %s: %s\n%s\n", sev, kw_finding_kind_name(f->kind),
                     f->summary, f->details);
    }
}

/* Emit a multi-line string as a JSON array with one element per line. */
static void json_lines(kw_strbuf *out, const char *text)
{
    kw_sb_puts(out, "[");
    char *copy = kw_xstrdup(text);
    char *save = NULL;
    int n = 0;
    for (char *line = strtok_r(copy, "\n", &save); line != NULL;
         line = strtok_r(NULL, "\n", &save)) {
        line += strspn(line, " ");
        kw_sb_puts(out, n++ ? "," : "");
        kw_sb_json_string(out, line);
    }
    free(copy);
    kw_sb_puts(out, "]");
}

void kw_render_json(const kw_report *r, kw_strbuf *out)
{
    kw_sb_printf(out,
                 "{\"time_ns\":%" PRIu64 ",\"scan_ns\":%" PRIu64
                 ",\"processes\":%zu,\"threads\":%zu,\"connections\":%zu,\"worst\":",
                 r->snap->time_ns, r->snap->scan_ns, r->snap->n_procs,
                 r->snap->n_threads, r->snap->n_conns);
    int worst = kw_findings_worst(r->findings);
    kw_sb_json_string(out, worst < 0 ? "none" : kw_severity_name((kw_severity)worst));

    kw_sb_puts(out, ",\"findings\":[");
    for (size_t i = 0; i < r->findings->n; i++) {
        const kw_finding *f = &r->findings->items[i];
        kw_sb_puts(out, i ? ",{\"kind\":" : "{\"kind\":");
        kw_sb_json_string(out, kw_finding_kind_name(f->kind));
        kw_sb_puts(out, ",\"severity\":");
        kw_sb_json_string(out, kw_severity_name(kw_finding_severity(f->kind)));
        kw_sb_puts(out, ",\"key\":");
        kw_sb_json_string(out, f->key);
        kw_sb_puts(out, ",\"summary\":");
        kw_sb_json_string(out, f->summary);
        kw_sb_puts(out, ",\"details\":");
        json_lines(out, f->details);
        kw_sb_puts(out, ",\"processes\":[");
        for (size_t j = 0; j < f->n_procs; j++) {
            kw_sb_puts(out, j ? "," : "");
            kw_sb_json_string(out, f->procs[j]);
        }
        kw_sb_puts(out, "]}");
    }

    kw_sb_puts(out, "],\"graph\":{\"nodes\":[");
    for (size_t i = 0; i < r->graph->n_nodes; i++) {
        kw_sb_puts(out, i ? "," : "");
        kw_sb_json_string(out, r->graph->nodes[i].name);
    }
    kw_sb_puts(out, "],\"edges\":[");
    for (size_t i = 0; i < r->graph->n_edges; i++) {
        const kw_gedge *e = &r->graph->edges[i];
        kw_sb_puts(out, i ? ",{\"from\":" : "{\"from\":");
        kw_sb_json_string(out, r->graph->nodes[e->from].name);
        kw_sb_puts(out, ",\"to\":");
        kw_sb_json_string(out, r->graph->nodes[e->to].name);
        kw_sb_printf(out, ",\"observed\":%s,\"connected\":%s}",
                     e->observed ? "true" : "false", e->connected ? "true" : "false");
    }
    kw_sb_puts(out, "]}}\n");
}

/* DOT double-quoted string: escape quotes and backslashes. */
static void dot_string(kw_strbuf *out, const char *s)
{
    kw_sb_puts(out, "\"");
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') {
            kw_sb_puts(out, "\\");
        }
        char one[2] = {*s, '\0'};
        kw_sb_puts(out, one);
    }
    kw_sb_puts(out, "\"");
}

/* Worst severity of any finding that involves process `name`, or -1. */
static int node_severity(const kw_findings *f, const char *name)
{
    int worst = -1;
    for (size_t i = 0; i < f->n; i++) {
        for (size_t j = 0; j < f->items[i].n_procs; j++) {
            int sev = (int)kw_finding_severity(f->items[i].kind);
            if (sev > worst && strcmp(f->items[i].procs[j], name) == 0) {
                worst = sev;
            }
        }
    }
    return worst;
}

void kw_render_dot(const kw_report *r, kw_strbuf *out)
{
    const kw_graph *g = r->graph;
    bool *used = kw_xcalloc(g->n_nodes, sizeof(bool));
    for (size_t i = 0; i < g->n_edges; i++) {
        used[g->edges[i].from] = true;
        used[g->edges[i].to] = true;
    }
    kw_sb_puts(out, "digraph knotwatch {\n"
                    "  rankdir=BT;\n"
                    "  label=\"send graph: client -> server "
                    "(solid = send observed, dashed = connection only)\";\n"
                    "  node [shape=box, style=\"rounded,filled\", "
                    "fillcolor=white, fontname=\"Helvetica\"];\n"
                    "  edge [fontname=\"Helvetica\"];\n");
    for (size_t i = 0; i < g->n_nodes; i++) {
        if (!used[i]) {
            continue; /* processes that talk to nobody only add clutter */
        }
        static const char *const fill[] = {"#fff3bf", "#ffd8a8", "#ffa8a8"};
        int sev = node_severity(r->findings, g->nodes[i].name);
        kw_sb_puts(out, "  ");
        dot_string(out, g->nodes[i].name);
        if (sev >= 0) {
            kw_sb_printf(out, " [fillcolor=\"%s\"]", fill[sev]);
        }
        kw_sb_puts(out, ";\n");
    }
    for (size_t i = 0; i < g->n_edges; i++) {
        const kw_gedge *e = &g->edges[i];
        kw_sb_puts(out, "  ");
        dot_string(out, g->nodes[e->from].name);
        kw_sb_puts(out, " -> ");
        dot_string(out, g->nodes[e->to].name);
        kw_sb_puts(out, e->observed ? ";\n" : " [style=dashed, color=gray50];\n");
    }
    kw_sb_puts(out, "}\n");
    free(used);
}
