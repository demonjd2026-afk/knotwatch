#include "kw/contract.h"

#include <errno.h>
#include <fnmatch.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "kw/util.h"

void kw_contract_init(kw_contract *c)
{
    memset(c, 0, sizeof(*c));
}

void kw_contract_free(kw_contract *c)
{
    for (size_t i = 0; i < c->n_layers; i++) {
        free(c->layers[i].glob);
    }
    for (size_t i = 0; i < c->n_ignores; i++) {
        free(c->ignores[i].client_glob);
        free(c->ignores[i].server_glob);
    }
    free(c->layers);
    free(c->ignores);
    memset(c, 0, sizeof(*c));
}

static void add_layer(kw_contract *c, int layer, const char *glob)
{
    void *items = c->layers;
    kw_grow(&items, &c->cap_layers, c->n_layers + 1, sizeof(kw_layer_rule));
    c->layers = items;
    c->layers[c->n_layers].layer = layer;
    c->layers[c->n_layers].glob = kw_xstrdup(glob);
    c->n_layers++;
}

static void add_ignore(kw_contract *c, const char *client, const char *server)
{
    void *items = c->ignores;
    kw_grow(&items, &c->cap_ignores, c->n_ignores + 1, sizeof(kw_ignore_rule));
    c->ignores = items;
    c->ignores[c->n_ignores].client_glob = kw_xstrdup(client);
    c->ignores[c->n_ignores].server_glob = kw_xstrdup(server);
    c->n_ignores++;
}

/* Returns 0 on success, -1 if the line is malformed. */
static int parse_line(char *text, kw_contract *c)
{
    char *save = NULL;
    char *word = strtok_r(text, " \t", &save);
    if (word == NULL) {
        return 0;
    }
    if (strcmp(word, "strict") == 0) {
        c->strict = true;
        return strtok_r(NULL, " \t", &save) == NULL ? 0 : -1;
    }
    if (strcmp(word, "layer") == 0) {
        char *num = strtok_r(NULL, " \t", &save);
        if (num == NULL) {
            return -1;
        }
        errno = 0;
        char *end = NULL;
        long layer = strtol(num, &end, 10);
        if (errno != 0 || *end != '\0' || layer < 0 || layer > INT_MAX) {
            return -1;
        }
        int globs = 0;
        for (char *g = strtok_r(NULL, " \t", &save); g != NULL;
             g = strtok_r(NULL, " \t", &save)) {
            add_layer(c, (int)layer, g);
            globs++;
        }
        return globs > 0 ? 0 : -1;
    }
    if (strcmp(word, "ignore") == 0) {
        char *client = strtok_r(NULL, " \t", &save);
        char *server = strtok_r(NULL, " \t", &save);
        if (client == NULL || server == NULL || strtok_r(NULL, " \t", &save) != NULL) {
            return -1;
        }
        add_ignore(c, client, server);
        return 0;
    }
    return -1;
}

int kw_contract_read(FILE *in, kw_contract *c, char *err, size_t err_size)
{
    char *line = NULL;
    size_t cap = 0;
    unsigned long lineno = 0;
    int rc = 0;

    while (getline(&line, &cap, in) != -1) {
        lineno++;
        line[strcspn(line, "#\r\n")] = '\0'; /* strip comment and newline */
        if (parse_line(line, c) != 0) {
            snprintf(err, err_size,
                     "line %lu: expected 'layer <n> <glob>...', "
                     "'ignore <client-glob> <server-glob>' or 'strict'",
                     lineno);
            rc = -1;
            break;
        }
    }
    free(line);
    if (rc == 0 && ferror(in)) {
        snprintf(err, err_size, "read error");
        rc = -1;
    }
    return rc;
}

int kw_contract_layer(const kw_contract *c, const char *name)
{
    for (size_t i = 0; i < c->n_layers; i++) {
        if (fnmatch(c->layers[i].glob, name, 0) == 0) {
            return c->layers[i].layer;
        }
    }
    return -1;
}

bool kw_contract_ignored(const kw_contract *c, const char *client, const char *server)
{
    if (c == NULL) {
        return false;
    }
    for (size_t i = 0; i < c->n_ignores; i++) {
        if (fnmatch(c->ignores[i].client_glob, client, 0) == 0 &&
            fnmatch(c->ignores[i].server_glob, server, 0) == 0) {
            return true;
        }
    }
    return false;
}
